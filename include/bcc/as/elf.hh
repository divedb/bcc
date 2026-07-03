#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "bcc/as/section.hh"
#include "bcc/as/symbol.hh"

namespace bcc::as {

//===----------------------------------------------------------------------===//
// ELF64 constants (System V x86-64). Defined locally so we depend on no system
// <elf.h>.
//===----------------------------------------------------------------------===//

// e_ident indices and values.
inline constexpr uint8_t kElfMag0 = 0x7f;
inline constexpr uint8_t kElfClass64 = 2;
inline constexpr uint8_t kElfData2Lsb = 1;
inline constexpr uint8_t kEvCurrent = 1;
inline constexpr uint8_t kElfOsabiSysv = 0;

// e_type.
inline constexpr uint16_t kEtRel = 1;
// e_machine.
inline constexpr uint16_t kEmX8664 = 62;

// Section types (sh_type).
inline constexpr uint32_t kShtNull = 0;
inline constexpr uint32_t kShtProgbits = 1;
inline constexpr uint32_t kShtSymtab = 2;
inline constexpr uint32_t kShtStrtab = 3;
inline constexpr uint32_t kShtRela = 4;
inline constexpr uint32_t kShtNobits = 8;

// Section flags (sh_flags).
inline constexpr uint64_t kShfWrite = 0x1;
inline constexpr uint64_t kShfAlloc = 0x2;
inline constexpr uint64_t kShfExecinstr = 0x4;
inline constexpr uint64_t kShfMerge = 0x10;
inline constexpr uint64_t kShfStrings = 0x20;

// Special section indices.
inline constexpr uint16_t kShnUndef = 0;
inline constexpr uint16_t kShnAbs = 0xfff1;
inline constexpr uint16_t kShnCommon = 0xfff2;

// Symbol binding / type (st_info).
inline constexpr uint8_t kStbLocal = 0;
inline constexpr uint8_t kStbGlobal = 1;
inline constexpr uint8_t kStbWeak = 2;
inline constexpr uint8_t kSttNotype = 0;
inline constexpr uint8_t kSttObject = 1;
inline constexpr uint8_t kSttFunc = 2;
inline constexpr uint8_t kSttSection = 3;

//===----------------------------------------------------------------------===//
// ELF64 on-disk structures.
//===----------------------------------------------------------------------===//

struct Elf64Ehdr {
  uint8_t e_ident[16];
  uint16_t e_type;
  uint16_t e_machine;
  uint32_t e_version;
  uint64_t e_entry;
  uint64_t e_phoff;
  uint64_t e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize;
  uint16_t e_phentsize;
  uint16_t e_phnum;
  uint16_t e_shentsize;
  uint16_t e_shnum;
  uint16_t e_shstrndx;
};

struct Elf64Shdr {
  uint32_t sh_name;
  uint32_t sh_type;
  uint64_t sh_flags;
  uint64_t sh_addr;
  uint64_t sh_offset;
  uint64_t sh_size;
  uint32_t sh_link;
  uint32_t sh_info;
  uint64_t sh_addralign;
  uint64_t sh_entsize;
};

struct Elf64Sym {
  uint32_t st_name;
  uint8_t st_info;
  uint8_t st_other;
  uint16_t st_shndx;
  uint64_t st_value;
  uint64_t st_size;
};

struct Elf64Rela {
  uint64_t r_offset;
  uint64_t r_info;  // (sym_index << 32) | type
  int64_t r_addend;
};

/// \brief Serializes assembled sections + symbols into an ELF64 `ET_REL`
///        relocatable object.
///
/// Also performs final fixup resolution: fixups that reduce to a constant are
/// patched into the section bytes; the rest are lowered to `.rela.*` entries.
class ElfWriter {
 public:
  /// \param sections The content sections in emission order (`.text`, `.data`,
  ///                 ...). Their bytes may be patched during \ref Build.
  /// \param symtab   The symbol table; symbols are assigned final ELF indices.
  ElfWriter(std::vector<Section*> sections, SymbolTable& symtab)
      : sections_(std::move(sections)), symtab_(symtab) {}

  /// Builds the complete object image. \return true on success; on failure
  /// \ref error() describes the problem (e.g. an unresolvable expression).
  bool Build(std::vector<uint8_t>& out);

  /// Convenience: build and write to \p path. \return true on success.
  bool WriteFile(const std::string& path);

  const std::string& error() const noexcept { return error_; }

 private:
  Section* MakeSectionSymbolOwner();
  void Fail(std::string msg) { error_ = std::move(msg); }

  std::vector<Section*> sections_;
  SymbolTable& symtab_;
  std::string error_;

  // Section symbols (STT_SECTION), owned here for stable addresses.
  std::deque<Symbol> section_symbols_;
};

}  // namespace bcc::as
