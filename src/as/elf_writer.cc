#include "bcc/as/elf.hh"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <unordered_map>

namespace bcc::as {

namespace {

/// Growable little-endian byte sink for building the object image.
class Out {
 public:
  size_t size() const noexcept { return buf_.size(); }
  std::vector<uint8_t>& buf() noexcept { return buf_; }

  void U8(uint8_t v) { buf_.push_back(v); }
  void U16(uint16_t v) { Put(v, 2); }
  void U32(uint32_t v) { Put(v, 4); }
  void U64(uint64_t v) { Put(v, 8); }
  void I64(int64_t v) { Put(static_cast<uint64_t>(v), 8); }

  void Bytes(const uint8_t* p, size_t n) { buf_.insert(buf_.end(), p, p + n); }
  void Bytes(const std::vector<uint8_t>& v) { Bytes(v.data(), v.size()); }

  void Zero(size_t n) { buf_.insert(buf_.end(), n, 0); }

  void AlignTo(size_t a) {
    while (a > 1 && (buf_.size() % a) != 0) buf_.push_back(0);
  }

 private:
  void Put(uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) buf_.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  std::vector<uint8_t> buf_;
};

/// Interned NUL-terminated string blob (`.strtab`/`.shstrtab`).
class StringTable {
 public:
  StringTable() { data_.push_back(0); }  // index 0 is the empty string

  uint32_t Add(const std::string& s) {
    if (s.empty()) return 0;
    auto it = map_.find(s);
    if (it != map_.end()) return it->second;
    uint32_t off = static_cast<uint32_t>(data_.size());
    data_.insert(data_.end(), s.begin(), s.end());
    data_.push_back(0);
    map_.emplace(s, off);
    return off;
  }

  const std::vector<uint8_t>& data() const noexcept { return data_; }

 private:
  std::vector<uint8_t> data_;
  std::unordered_map<std::string, uint32_t> map_;
};

RelType SelectReloc(uint8_t size, bool pcrel, RelModifier mod, bool sext,
                    bool is_branch) {
  if (pcrel) {
    switch (mod) {
      case RelModifier::kPLT:
        return RelType::kPLT32;
      case RelModifier::kGOTPCREL:
        return RelType::kGOTPCREL;
      default:
        // An unresolved branch to an external/global target links through the
        // PLT, matching modern gas/llvm-mc; other PC-relative refs use PC32.
        if (is_branch && size == 4) return RelType::kPLT32;
        return size == 8 ? RelType::kPC64 : RelType::kPC32;
    }
  }
  switch (size) {
    case 8:
      return RelType::k64;
    case 4:
      return sext ? RelType::k32S : RelType::k32;
    case 2:
      return RelType::k16;
    default:
      return RelType::k8;
  }
}

}  // namespace

bool ElfWriter::Build(std::vector<uint8_t>& out) {
  // (a) Assign content-section indices [1..N] and create their STT_SECTION
  // symbols.
  uint32_t next_shndx = 1;
  for (Section* s : sections_) {
    s->elf_index = next_shndx++;
    section_symbols_.push_back(Symbol{});
    Symbol& ss = section_symbols_.back();
    ss.name.clear();  // section symbols are unnamed (st_name = 0)
    ss.section = s;
    ss.offset = 0;
    ss.binding = Binding::kLocal;
    ss.type = SymType::kSection;
    ss.defined = true;
    s->section_symbol = &ss;
  }

  // (b) An undefined, referenced symbol is implicitly external (global).
  for (Symbol* sym : symtab_.symbols()) {
    if (!sym->defined && !sym->is_common && sym->binding == Binding::kLocal)
      sym->binding = Binding::kGlobal;
  }

  // (c) Order symbols: null, locals (section symbols first), then globals/weak.
  std::vector<Symbol*> ordered;
  ordered.push_back(nullptr);  // index 0: the null symbol

  auto want_emit = [](Symbol* s) {
    // Temporary local labels (the `.L` prefix, by gas/LLVM convention) are not
    // written to the object symbol table: local symbol references are lowered
    // through their section symbol, so these are never needed by relocations.
    if (s->binding == Binding::kLocal && s->name.rfind(".L", 0) == 0)
      return false;
    return s->defined || s->is_common || s->referenced;
  };

  // Locals: section symbols, then local user symbols.
  for (Symbol& ss : section_symbols_) ordered.push_back(&ss);
  for (Symbol* sym : symtab_.symbols())
    if (sym->binding == Binding::kLocal && want_emit(sym)) ordered.push_back(sym);

  uint32_t first_global = static_cast<uint32_t>(ordered.size());

  // Globals / weaks (defined, undefined, or common).
  for (Symbol* sym : symtab_.symbols())
    if (sym->binding != Binding::kLocal && want_emit(sym)) ordered.push_back(sym);

  for (uint32_t i = 0; i < ordered.size(); ++i)
    if (ordered[i]) ordered[i]->elf_index = i;

  // (d) Resolve fixups: patch constants in place, collect the rest as relocs.
  std::unordered_map<Section*, std::vector<Elf64Rela>> relocs;
  for (Section* s : sections_) {
    for (const Fixup& f : s->fixups()) {
      const MCValue& v = f.value;

      // Symbol difference A - B: only same-section, both-defined is supported;
      // it folds to a position-independent constant.
      if (v.sym && v.sym2) {
        if (!v.sym->defined || !v.sym2->defined ||
            v.sym->section != v.sym2->section) {
          Fail("unsupported symbol difference in expression");
          return false;
        }
        int64_t c = static_cast<int64_t>(v.sym->offset) -
                    static_cast<int64_t>(v.sym2->offset) + v.addend;
        s->PatchLE(f.offset, static_cast<uint64_t>(c), f.size);
        continue;
      }

      if (v.IsConstant()) {
        s->PatchLE(f.offset, static_cast<uint64_t>(v.addend), f.size);
        continue;
      }

      Symbol* S = v.sym;

      // An absolute (section-less) defined symbol, e.g. from `.set`, folds to
      // its constant value.
      if (S->defined && S->section == nullptr &&
          v.mod == RelModifier::kNone && !f.pcrel) {
        s->PatchLE(f.offset, static_cast<uint64_t>(S->offset + v.addend),
                   f.size);
        continue;
      }

      bool resolvable = f.pcrel && S->defined && S->section == s &&
                        S->binding == Binding::kLocal &&
                        v.mod == RelModifier::kNone;
      if (resolvable) {
        int64_t disp =
            static_cast<int64_t>(S->offset) + v.addend -
            static_cast<int64_t>(f.offset + f.size + f.trailing);
        s->PatchLE(f.offset, static_cast<uint64_t>(disp), f.size);
        continue;
      }

      // Lower to a relocation. Local defined symbols are referenced through
      // their section symbol with the offset folded into the addend.
      Symbol* target;
      int64_t base_addend;
      if (S->defined && S->binding == Binding::kLocal && !S->is_common &&
          S->section != nullptr) {
        target = S->section->section_symbol;
        base_addend = static_cast<int64_t>(S->offset);
      } else {
        target = S;
        base_addend = 0;
      }
      int64_t addend = v.addend + base_addend;
      if (f.pcrel) addend -= static_cast<int64_t>(f.size + f.trailing);

      RelType rt =
          SelectReloc(f.size, f.pcrel, v.mod, f.sign_extend, f.is_branch);
      Elf64Rela r;
      r.r_offset = f.offset;
      r.r_info = (static_cast<uint64_t>(target->elf_index) << 32) |
                 static_cast<uint32_t>(rt);
      r.r_addend = addend;
      relocs[s].push_back(r);
    }
  }

  // (e) Assign remaining section indices: .rela.* (only where relocs exist),
  // then .symtab, .strtab, .shstrtab.
  std::vector<Section*> reloc_sections;
  for (Section* s : sections_) {
    if (!relocs[s].empty()) {
      s->rela_elf_index = next_shndx++;
      reloc_sections.push_back(s);
    }
  }
  uint32_t symtab_ndx = next_shndx++;
  uint32_t strtab_ndx = next_shndx++;
  uint32_t shstrtab_ndx = next_shndx++;
  uint32_t shnum = next_shndx;

  // (f) Build string tables and the symbol table body.
  StringTable strtab, shstrtab;
  Out symtab;
  symtab.Zero(24);  // symbol 0 is all zero
  for (size_t i = 1; i < ordered.size(); ++i) {
    Symbol* sym = ordered[i];
    uint32_t name_off = strtab.Add(sym->name);
    uint8_t bind = sym->binding == Binding::kLocal    ? kStbLocal
                   : sym->binding == Binding::kWeak   ? kStbWeak
                                                      : kStbGlobal;
    uint8_t type = sym->type == SymType::kSection ? kSttSection
                   : sym->type == SymType::kFunc  ? kSttFunc
                   : sym->type == SymType::kObject ? kSttObject
                                                   : kSttNotype;
    uint16_t shndx;
    uint64_t value;
    uint64_t size = sym->size;
    if (sym->is_common) {
      shndx = kShnCommon;
      value = sym->common_align ? sym->common_align : 1;
      size = sym->common_size;
    } else if (!sym->defined) {
      shndx = kShnUndef;
      value = 0;
    } else if (sym->section == nullptr) {
      shndx = kShnAbs;  // .set to a constant
      value = sym->offset;
    } else {
      shndx = static_cast<uint16_t>(sym->section->elf_index);
      value = sym->offset;
    }
    symtab.U32(name_off);
    symtab.U8(static_cast<uint8_t>((bind << 4) | type));
    symtab.U8(0);  // st_other
    symtab.U16(shndx);
    symtab.U64(value);
    symtab.U64(size);
  }
  uint32_t nsyms = static_cast<uint32_t>(ordered.size());

  // Section-header name offsets.
  uint32_t sh_name_symtab = shstrtab.Add(".symtab");
  uint32_t sh_name_strtab = shstrtab.Add(".strtab");
  uint32_t sh_name_shstrtab = shstrtab.Add(".shstrtab");
  std::vector<uint32_t> content_name(sections_.size());
  for (size_t i = 0; i < sections_.size(); ++i)
    content_name[i] = shstrtab.Add(sections_[i]->name());
  std::unordered_map<Section*, uint32_t> rela_name;
  for (Section* s : reloc_sections)
    rela_name[s] = shstrtab.Add(".rela" + s->name());

  // Serialize relocation section bodies.
  std::unordered_map<Section*, std::vector<uint8_t>> rela_bytes;
  for (Section* s : reloc_sections) {
    Out o;
    for (const Elf64Rela& r : relocs[s]) {
      o.U64(r.r_offset);
      o.U64(r.r_info);
      o.I64(r.r_addend);
    }
    rela_bytes[s] = std::move(o.buf());
  }

  // (g) Lay out the file: ELF header, section bodies, then section headers.
  Out file;
  file.Zero(64);  // ELF header, back-patched below

  struct Placed {
    uint64_t offset = 0;
    uint64_t size = 0;
  };
  std::unordered_map<Section*, Placed> content_placed;
  for (Section* s : sections_) {
    Placed p;
    if (s->is_bss()) {
      p.offset = file.size();  // NOBITS occupies no file space
      p.size = s->Offset();
    } else {
      file.AlignTo(s->align() ? s->align() : 1);
      p.offset = file.size();
      p.size = s->data().size();
      file.Bytes(s->data());
    }
    content_placed[s] = p;
  }

  std::unordered_map<Section*, Placed> rela_placed;
  for (Section* s : reloc_sections) {
    file.AlignTo(8);
    Placed p{file.size(), rela_bytes[s].size()};
    file.Bytes(rela_bytes[s]);
    rela_placed[s] = p;
  }

  file.AlignTo(8);
  Placed symtab_placed{file.size(), symtab.size()};
  file.Bytes(symtab.buf());

  Placed strtab_placed{file.size(), strtab.data().size()};
  file.Bytes(strtab.data());

  Placed shstrtab_placed{file.size(), shstrtab.data().size()};
  file.Bytes(shstrtab.data());

  file.AlignTo(8);
  uint64_t shoff = file.size();

  // Section header table.
  auto write_shdr = [&](uint32_t name, uint32_t type, uint64_t flags,
                        uint64_t addr, uint64_t offset, uint64_t size,
                        uint32_t link, uint32_t info, uint64_t align,
                        uint64_t entsize) {
    file.U32(name);
    file.U32(type);
    file.U64(flags);
    file.U64(addr);
    file.U64(offset);
    file.U64(size);
    file.U32(link);
    file.U32(info);
    file.U64(align);
    file.U64(entsize);
  };

  write_shdr(0, kShtNull, 0, 0, 0, 0, 0, 0, 0, 0);  // section 0
  for (size_t i = 0; i < sections_.size(); ++i) {
    Section* s = sections_[i];
    write_shdr(content_name[i], s->sh_type(), s->sh_flags(), 0,
               content_placed[s].offset, content_placed[s].size, 0, 0,
               s->align() ? s->align() : 1, 0);
  }
  for (Section* s : reloc_sections) {
    write_shdr(rela_name[s], kShtRela, 0, 0, rela_placed[s].offset,
               rela_placed[s].size, symtab_ndx, s->elf_index, 8, 24);
  }
  write_shdr(sh_name_symtab, kShtSymtab, 0, 0, symtab_placed.offset,
             symtab_placed.size, strtab_ndx, first_global, 8, 24);
  write_shdr(sh_name_strtab, kShtStrtab, 0, 0, strtab_placed.offset,
             strtab_placed.size, 0, 0, 1, 0);
  write_shdr(sh_name_shstrtab, kShtStrtab, 0, 0, shstrtab_placed.offset,
             shstrtab_placed.size, 0, 0, 1, 0);

  (void)nsyms;

  // Back-patch the ELF header.
  Out eh;
  eh.U8(kElfMag0);
  eh.U8('E');
  eh.U8('L');
  eh.U8('F');
  eh.U8(kElfClass64);
  eh.U8(kElfData2Lsb);
  eh.U8(kEvCurrent);
  eh.U8(kElfOsabiSysv);
  eh.Zero(8);         // EI_ABIVERSION + padding
  eh.U16(kEtRel);     // e_type
  eh.U16(kEmX8664);   // e_machine
  eh.U32(kEvCurrent); // e_version
  eh.U64(0);          // e_entry
  eh.U64(0);          // e_phoff
  eh.U64(shoff);      // e_shoff
  eh.U32(0);          // e_flags
  eh.U16(64);         // e_ehsize
  eh.U16(0);          // e_phentsize
  eh.U16(0);          // e_phnum
  eh.U16(64);         // e_shentsize
  eh.U16(static_cast<uint16_t>(shnum));
  eh.U16(static_cast<uint16_t>(shstrtab_ndx));
  std::memcpy(file.buf().data(), eh.buf().data(), 64);

  out = std::move(file.buf());
  return true;
}

bool ElfWriter::WriteFile(const std::string& path) {
  std::vector<uint8_t> bytes;
  if (!Build(bytes)) return false;
  std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
  if (!ofs) {
    Fail("cannot open output file '" + path + "'");
    return false;
  }
  ofs.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(ofs);
}

}  // namespace bcc::as
