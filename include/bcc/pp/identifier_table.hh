#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "bcc/lex/token_kind.hh"

namespace bcc {

/// \brief Identifies preprocessor keywords.
///
/// A preprocessor keyword is an identifier with special meaning to the
/// preprocessor: a directive name (#include, #define, ...) or a #if-expression
/// operator (defined, __has_include). This is orthogonal to whether the same
/// spelling is also a C keyword; "if", for instance, is both.
enum class PPKeyword : uint8_t {
  kNotKeyword = 0,
#define PP_KEYWORD(id, spelling) id,
#include "bcc/pp/pp_keywords.def"
#undef PP_KEYWORD
};

/// \brief Interned, per-spelling information about an identifier.
///
/// Exactly one IdentifierInfo exists for each distinct identifier spelling in a
/// translation unit. It is created and owned by an IdentifierTable and is
/// pointer-stable for the table's lifetime, so downstream machinery (macro
/// maps, the parser) may key off the IdentifierInfo* directly.
class IdentifierInfo {
 public:
  IdentifierInfo() = default;
  IdentifierInfo(const IdentifierInfo&) = delete;
  IdentifierInfo& operator=(const IdentifierInfo&) = delete;

  /// \brief The identifier's spelling. Valid for the owning table's lifetime.
  std::string_view GetName() const noexcept { return name_; }

  /// \brief The token kind this identifier lexes as.
  ///
  /// TokenKind::kIdentifier for an ordinary identifier, or the corresponding
  /// keyword kind (e.g. TokenKind::kInt) if the spelling is a C keyword.
  TokenKind GetTokenKind() const noexcept { return token_kind_; }

  /// \brief True if this identifier is a C keyword.
  bool IsKeyword() const noexcept { return IsKeywordKind(token_kind_); }

  /// \brief The preprocessor keyword this identifier names, or
  ///        PPKeyword::kNotKeyword if it is not a preprocessor keyword.
  PPKeyword GetPPKeyword() const noexcept { return pp_keyword_; }

  /// \brief True if a macro is currently #defined with this name.
  ///
  /// The preprocessor keeps this bit in sync with #define/#undef so the O(1)
  /// "should this identifier be expanded?" test needs only a single flag check.
  bool HasMacroDefinition() const noexcept { return has_macro_definition_; }
  void SetHasMacroDefinition(bool value) noexcept {
    has_macro_definition_ = value;
  }

  /// \brief True if this names a builtin macro (e.g. __LINE__, __FILE__).
  bool IsBuiltinMacro() const noexcept { return is_builtin_macro_; }
  void SetIsBuiltinMacro(bool value) noexcept { is_builtin_macro_ = value; }

 private:
  friend class IdentifierTable;

  std::string_view name_;
  TokenKind token_kind_ = TokenKind::kIdentifier;
  PPKeyword pp_keyword_ = PPKeyword::kNotKeyword;
  bool has_macro_definition_ = false;
  bool is_builtin_macro_ = false;
};

/// \brief Owns and interns the IdentifierInfo for every identifier spelling.
///
/// All C keywords and preprocessor keywords are pre-seeded at construction, so
/// keyword classification is a single lookup with no string comparison.
class IdentifierTable {
 public:
  IdentifierTable();

  IdentifierTable(const IdentifierTable&) = delete;
  IdentifierTable& operator=(const IdentifierTable&) = delete;

  /// \brief Returns the IdentifierInfo for \p name, interning it on first use.
  ///
  /// The returned reference is stable for the lifetime of this table.
  IdentifierInfo& Get(std::string_view name);

  /// \brief The number of interned identifiers, including pre-seeded keywords.
  std::size_t Size() const noexcept { return map_.size(); }

 private:
  void AddKeywords();

  /// Transparent hash so string_view lookups don't allocate a std::string.
  struct StringHash {
    using is_transparent = void;

    std::size_t operator()(std::string_view s) const noexcept {
      return std::hash<std::string_view>{}(s);
    }
  };

  /// Node-based storage: elements are pointer-stable across rehashes, and each
  /// IdentifierInfo::name_ points at its own stable key string.
  std::unordered_map<std::string, IdentifierInfo, StringHash, std::equal_to<>>
      map_;
};

}  // namespace bcc
