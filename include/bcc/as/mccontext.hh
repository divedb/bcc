#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "bcc/as/section.hh"
#include "bcc/as/symbol.hh"

namespace bcc::as {

/// \brief The assembler's mutable state: the section pool, the symbol table,
///        and the current section / location counter.
///
/// Assembly starts in `.text`, matching `gas`. Sections are created on demand
/// and retained in creation order, which is the order the ELF writer serializes
/// them.
class MCContext {
 public:
  MCContext();

  SymbolTable& symtab() noexcept { return symtab_; }

  /// Returns the section named \p name, creating it with the given ELF type and
  /// flags on first use.
  Section* GetOrCreateSection(std::string_view name, uint32_t sh_type,
                              uint64_t sh_flags);

  Section* text() { return GetOrCreateSection(".text", 1, 0x2 | 0x4); }
  Section* data() { return GetOrCreateSection(".data", 1, 0x1 | 0x2); }
  Section* rodata() { return GetOrCreateSection(".rodata", 1, 0x2); }
  Section* bss();

  void SwitchSection(Section* s) noexcept { current_ = s; }
  Section* current() const noexcept { return current_; }

  /// Defines symbol \p name at the current section and location. Returns the
  /// symbol, or nullptr if it was already defined (redefinition).
  Symbol* DefineLabel(std::string_view name);

  /// Relaxes intra-section `jmp`/`jcc` branches to defined local labels from
  /// 4-byte `rel32` down to 2-byte `rel8` wherever the displacement fits, then
  /// re-lays-out each section (updating all label and fixup offsets). Must run
  /// after parsing and before ELF emission.
  void RelaxBranches();

  /// All sections in creation order (for ELF emission).
  std::vector<Section*> sections() const;

 private:
  SymbolTable symtab_;
  std::vector<std::unique_ptr<Section>> sections_;
  std::unordered_map<std::string, Section*> section_map_;
  Section* current_ = nullptr;
};

}  // namespace bcc::as
