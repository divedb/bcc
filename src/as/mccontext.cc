#include "bcc/as/mccontext.hh"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "bcc/as/elf.hh"

namespace bcc::as {

namespace {

/// A relaxable branch discovered in a section: a `jmp`/`jcc` whose target is a
/// defined local label in the same section (so its displacement is known at
/// assembly time and could shrink to `rel8`).
struct BranchSpan {
  size_t fixup_index;   // index into the section's fixup list
  uint64_t old_start;   // instruction start offset (pre-relaxation)
  uint64_t old_size;    // opcode_len + 4
  uint64_t target_off;  // target label offset (pre-relaxation)
  int64_t addend;       // constant addend on the target
  uint8_t cc;           // condition code (jcc); ignored for jmp
  bool is_jmp;
  bool is_short = false;

  uint64_t Save() const { return is_short ? (old_size - 2) : 0; }
};

// Appends `n` bytes of multi-byte NOP padding (the canonical x86 sequences),
// used to pad executable-section alignment.
void EmitNops(std::vector<uint8_t>& out, uint64_t n) {
  static const std::vector<std::vector<uint8_t>> kNop = {
      {},
      {0x90},
      {0x66, 0x90},
      {0x0f, 0x1f, 0x00},
      {0x0f, 0x1f, 0x40, 0x00},
      {0x0f, 0x1f, 0x44, 0x00, 0x00},
      {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00},
      {0x0f, 0x1f, 0x80, 0x00, 0x00, 0x00, 0x00},
      {0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
      {0x66, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
  };
  while (n > 0) {
    uint64_t k = n < 9 ? n : 9;  // longest canonical NOP we emit is 9 bytes
    out.insert(out.end(), kNop[k].begin(), kNop[k].end());
    n -= k;
  }
}

// Relaxes one section in place: shrinks branch spans to rel8 where possible,
// recomputes alignment padding, rebuilds the byte buffer, and remaps every
// symbol/fixup offset via an explicit old->new map.
void RelaxSection(Section& sec, SymbolTable& symtab) {
  if (sec.is_bss()) return;
  const std::vector<uint8_t>& data = sec.data();
  std::vector<Fixup>& fixups = sec.fixups();

  std::vector<uint64_t> align_offs;
  for (const AlignPoint& a : sec.aligns()) align_offs.push_back(a.offset);

  // An alignment point strictly between [lo, hi) would make a branch's rel8
  // displacement depend on padding that itself moves; such branches are kept
  // rel32 so relaxation stays correct and simple.
  auto crosses_align = [&](uint64_t lo, uint64_t hi) {
    if (lo > hi) std::swap(lo, hi);
    for (uint64_t o : align_offs)
      if (o > lo && o < hi) return true;
    return false;
  };

  // (1) Discover relaxable branches: writer-resolvable local jmp/jcc that do
  // not cross an alignment point.
  std::vector<BranchSpan> brs;
  for (size_t i = 0; i < fixups.size(); ++i) {
    const Fixup& f = fixups[i];
    if (!f.is_branch || !f.pcrel || f.size != 4) continue;
    if (f.value.mod != RelModifier::kNone || f.value.sym2) continue;
    Symbol* t = f.value.sym;
    if (!t || !t->defined || t->section != &sec ||
        t->binding != Binding::kLocal)
      continue;

    uint64_t disp_off = f.offset;
    BranchSpan b;
    if (disp_off >= 1 && data[disp_off - 1] == 0xE9) {
      b.is_jmp = true;
      b.cc = 0;
      b.old_start = disp_off - 1;
    } else if (disp_off >= 2 && data[disp_off - 2] == 0x0F &&
               (data[disp_off - 1] & 0xF0) == 0x80) {
      b.is_jmp = false;
      b.cc = data[disp_off - 1] & 0x0F;
      b.old_start = disp_off - 2;
    } else {
      continue;  // e.g. call (0xE8) has no rel8 form
    }
    if (crosses_align(b.old_start, t->offset)) continue;
    b.fixup_index = i;
    b.old_size = disp_off + 4 - b.old_start;
    b.target_off = t->offset;
    b.addend = f.value.addend;
    brs.push_back(b);
  }
  if (brs.empty()) return;  // nothing to relax; layout (incl. alignment) intact

  std::sort(brs.begin(), brs.end(),
            [](const BranchSpan& a, const BranchSpan& b) {
              return a.old_start < b.old_start;
            });

  // Bytes removed by branches starting strictly before old offset x. Alignment
  // padding is excluded here: relaxable branches never cross an alignment, so
  // padding changes cancel out of their relative displacements.
  auto savings_before = [&](uint64_t x) -> uint64_t {
    uint64_t s = 0;
    for (const BranchSpan& b : brs)
      if (b.old_start < x) s += b.Save();
    return s;
  };

  // (2) Fixed-point shrink: mark a long branch short when its rel8 displacement
  // fits. Distances only shrink, so this is monotone and converges.
  bool changed = true;
  while (changed) {
    changed = false;
    for (BranchSpan& b : brs) {
      if (b.is_short) continue;
      uint64_t new_start = b.old_start - savings_before(b.old_start);
      uint64_t label_new = b.target_off - savings_before(b.target_off);
      if (b.old_start < b.target_off) label_new -= (b.old_size - 2);
      int64_t disp = static_cast<int64_t>(label_new) + b.addend -
                     static_cast<int64_t>(new_start + 2);
      if (disp >= -128 && disp <= 127) {
        b.is_short = true;
        changed = true;
      }
    }
  }

  // (3) Rebuild the byte buffer, recording the new offset of every old byte
  // boundary so symbols and fixups can be remapped exactly.
  std::vector<uint8_t> out;
  out.reserve(data.size());
  std::vector<uint64_t> new_of_old(data.size() + 1, 0);

  size_t bi = 0, ai = 0;
  const std::vector<AlignPoint>& aligns = sec.aligns();
  for (uint64_t p = 0; p < data.size();) {
    new_of_old[p] = out.size();
    // Alignment is processed before a branch at the same offset so that a
    // now-unaligned boundary still gets fresh padding.
    if (ai < aligns.size() && p == aligns[ai].offset) {
      const AlignPoint& a = aligns[ai];
      uint64_t old_pad = (a.boundary - (a.offset % a.boundary)) % a.boundary;
      uint64_t new_pad = (a.boundary - (out.size() % a.boundary)) % a.boundary;
      if (a.use_nop)
        EmitNops(out, new_pad);
      else
        out.insert(out.end(), new_pad, a.fill);
      p += old_pad;  // skip the stale padding in the old buffer
      ++ai;
    } else if (bi < brs.size() && p == brs[bi].old_start) {
      BranchSpan& b = brs[bi];
      if (b.is_short) {
        out.push_back(b.is_jmp ? 0xEB : static_cast<uint8_t>(0x70 | b.cc));
      } else if (b.is_jmp) {
        out.push_back(0xE9);
      } else {
        out.push_back(0x0F);
        out.push_back(static_cast<uint8_t>(0x80 | b.cc));
      }
      Fixup& f = fixups[b.fixup_index];
      f.offset = out.size();
      f.size = b.is_short ? 1 : 4;
      out.insert(out.end(), b.is_short ? 1 : 4, 0);  // displacement placeholder
      p += b.old_size;
      ++bi;
    } else {
      out.push_back(data[p]);
      ++p;
    }
  }
  new_of_old[data.size()] = out.size();

  // (4) Remap symbols and non-branch fixups through the map. Branch fixups were
  // already repositioned during the rebuild.
  std::vector<bool> is_branch_fixup(fixups.size(), false);
  for (const BranchSpan& b : brs) is_branch_fixup[b.fixup_index] = true;
  for (size_t i = 0; i < fixups.size(); ++i) {
    if (is_branch_fixup[i]) continue;
    fixups[i].offset = new_of_old[fixups[i].offset];
  }
  for (Symbol* sym : symtab.symbols()) {
    if (sym->section == &sec && sym->defined)
      sym->offset = new_of_old[sym->offset];
  }

  sec.data() = std::move(out);
}

}  // namespace

MCContext::MCContext() { current_ = text(); }

Section* MCContext::GetOrCreateSection(std::string_view name, uint32_t sh_type,
                                       uint64_t sh_flags) {
  std::string key(name);
  auto it = section_map_.find(key);
  if (it != section_map_.end()) return it->second;
  auto sec = std::make_unique<Section>(key, sh_type, sh_flags);
  Section* raw = sec.get();
  section_map_.emplace(key, raw);
  sections_.push_back(std::move(sec));
  return raw;
}

Section* MCContext::bss() {
  Section* s = GetOrCreateSection(".bss", kShtNobits, kShfWrite | kShfAlloc);
  s->set_is_bss(true);
  return s;
}

Symbol* MCContext::DefineLabel(std::string_view name) {
  Symbol* s = symtab_.GetOrCreate(name);
  if (s->defined) return nullptr;  // redefinition; caller reports
  s->section = current_;
  s->offset = current_->Offset();
  s->defined = true;
  s->referenced = true;
  return s;
}

void MCContext::RelaxBranches() {
  for (auto& s : sections_) RelaxSection(*s, symtab_);
}

std::vector<Section*> MCContext::sections() const {
  std::vector<Section*> out;
  out.reserve(sections_.size());
  for (const auto& s : sections_) out.push_back(s.get());
  return out;
}

}  // namespace bcc::as
