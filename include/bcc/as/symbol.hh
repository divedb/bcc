#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace bcc::as {

class Section;

/// \brief ELF symbol binding (`STB_*`).
enum class Binding : uint8_t { kLocal, kGlobal, kWeak };

/// \brief ELF symbol type (`STT_*`).
enum class SymType : uint8_t { kNoType, kObject, kFunc, kSection };

/// \brief A named assembler symbol: a label, an external reference, or a
///        `.comm`/`.set` symbol.
///
/// A symbol is *defined* once it is bound to a `section` + `offset` (by a label
/// or `.set`). Referencing a name before it is defined creates an undefined
/// entry that a later definition fills in; if no definition appears it becomes
/// an `SHN_UNDEF` entry the linker must resolve.
struct Symbol {
  std::string name;
  Section* section = nullptr;  ///< Defining section, or null if undefined.
  uint64_t offset = 0;         ///< Value within `section`.
  Binding binding = Binding::kLocal;
  SymType type = SymType::kNoType;
  uint64_t size = 0;  ///< From `.size`; 0 if unset.
  bool defined = false;

  /// `.comm`/`.lcomm`: a common (tentative) symbol of `common_size` bytes with
  /// `common_align` alignment. Emitted as `SHN_COMMON` (global) or placed in
  /// `.bss` (local).
  bool is_common = false;
  uint64_t common_size = 0;
  uint64_t common_align = 0;

  /// Set true when the name was mentioned by `.globl`/`.weak`/`.type` etc. even
  /// though it may still be undefined; keeps it in the emitted table.
  bool referenced = false;

  uint32_t elf_index = 0;  ///< Final `.symtab` index; filled during emission.
};

/// \brief Owns and interns all symbols by name.
class SymbolTable {
 public:
  /// Returns the symbol named \p name, creating an undefined entry if needed.
  Symbol* GetOrCreate(std::string_view name) {
    auto it = map_.find(std::string(name));
    if (it != map_.end()) return it->second;
    auto sym = std::make_unique<Symbol>();
    sym->name = std::string(name);
    Symbol* raw = sym.get();
    order_.push_back(raw);
    map_.emplace(raw->name, raw);
    storage_.push_back(std::move(sym));
    return raw;
  }

  /// Returns the symbol named \p name, or nullptr if it does not exist.
  Symbol* Lookup(std::string_view name) const {
    auto it = map_.find(std::string(name));
    return it == map_.end() ? nullptr : it->second;
  }

  /// All symbols in creation order.
  const std::vector<Symbol*>& symbols() const noexcept { return order_; }

 private:
  std::vector<std::unique_ptr<Symbol>> storage_;
  std::vector<Symbol*> order_;
  std::unordered_map<std::string, Symbol*> map_;
};

}  // namespace bcc::as
