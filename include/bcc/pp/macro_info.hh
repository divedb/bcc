#pragma once

#include <cstdint>
#include <vector>

#include "bcc/basic/source_location.hh"
#include "bcc/lex/token.hh"

namespace bcc {

class IdentifierInfo;

/// \brief The definition of a single macro (the tokens a #define expands to).
///
/// One MacroInfo exists per #define. Both object-like (#define X ...) and
/// function-like (#define F(a, b) ...) macros are represented; the latter
/// additionally carry a parameter list and, for variadic macros, a trailing
/// __VA_ARGS__ parameter.
class MacroInfo {
 public:
  explicit MacroInfo(SourceLocation definition_loc)
      : definition_loc_(definition_loc) {}

  MacroInfo(const MacroInfo&) = delete;
  MacroInfo& operator=(const MacroInfo&) = delete;

  //===--------------------------------------------------------------------===//
  // Definition state.
  //===--------------------------------------------------------------------===//

  void AddReplacementToken(const Token& token) {
    replacement_tokens_.push_back(token);
  }

  void SetDefinitionEndLoc(SourceLocation loc) noexcept {
    definition_end_loc_ = loc;
  }

  SourceLocation GetDefinitionLoc() const noexcept { return definition_loc_; }
  SourceLocation GetDefinitionEndLoc() const noexcept {
    return definition_end_loc_;
  }

  const std::vector<Token>& GetReplacementTokens() const noexcept {
    return replacement_tokens_;
  }

  unsigned GetNumTokens() const noexcept {
    return static_cast<unsigned>(replacement_tokens_.size());
  }

  bool IsFunctionLike() const noexcept { return is_function_like_; }
  bool IsObjectLike() const noexcept { return !is_function_like_; }
  void SetIsFunctionLike() noexcept { is_function_like_ = true; }

  /// True if this macro takes a variable number of arguments (has `...`), in
  /// which case the last entry of the parameter list is the __VA_ARGS__
  /// identifier.
  bool IsVariadic() const noexcept { return is_variadic_; }
  void SetIsVariadic(bool value) noexcept { is_variadic_ = value; }

  /// True if the replacement list contains a `##` paste operator. Cached at
  /// definition time so object-like macros without pasting can skip the
  /// substitution pass entirely.
  bool HasPasteOperator() const noexcept { return has_paste_; }
  void SetHasPasteOperator(bool value) noexcept { has_paste_ = value; }

  //===--------------------------------------------------------------------===//
  // Parameters (function-like macros).
  //===--------------------------------------------------------------------===//

  void AddParameter(IdentifierInfo* param) { parameters_.push_back(param); }

  const std::vector<IdentifierInfo*>& GetParameters() const noexcept {
    return parameters_;
  }

  unsigned GetNumParams() const noexcept {
    return static_cast<unsigned>(parameters_.size());
  }

  /// \brief The index of parameter \p ii, or -1 if \p ii is not a parameter.
  int GetParameterIndex(const IdentifierInfo* ii) const noexcept {
    for (unsigned i = 0; i < parameters_.size(); ++i) {
      if (parameters_[i] == ii) return static_cast<int>(i);
    }

    return -1;
  }

  //===--------------------------------------------------------------------===//
  // Expansion state.
  //===--------------------------------------------------------------------===//

  /// \brief Whether this macro may currently be expanded.
  ///
  /// A macro is disabled while it is being expanded so that a reference to it
  /// within its own expansion does not recurse (e.g. #define X X). See also
  /// TokenFlag::kDisableExpand, which paints individual tokens.
  bool IsEnabled() const noexcept { return !is_disabled_; }
  void EnableExpansion() noexcept { is_disabled_ = false; }
  void DisableExpansion() noexcept { is_disabled_ = true; }

  bool IsUsed() const noexcept { return is_used_; }
  void SetIsUsed(bool used) noexcept { is_used_ = used; }

  /// \brief Whether this is a compiler builtin macro (e.g. __LINE__, __FILE__)
  ///        expanded programmatically rather than from a token list.
  bool IsBuiltinMacro() const noexcept { return is_builtin_; }
  void SetIsBuiltinMacro(bool value) noexcept { is_builtin_ = value; }

 private:
  SourceLocation definition_loc_;
  SourceLocation definition_end_loc_;
  std::vector<Token> replacement_tokens_;
  std::vector<IdentifierInfo*> parameters_;

  bool is_function_like_ = false;
  bool is_variadic_ = false;
  bool has_paste_ = false;
  bool is_disabled_ = false;
  bool is_used_ = false;
  bool is_builtin_ = false;
};

/// \brief One node in the per-identifier history of #define / #undef.
///
/// The most recent directive for a name is the head; earlier ones are reached
/// through GetPrevious(). Keeping the history (rather than a single current
/// definition) leaves room for reconstructing macro state at a source location
/// and for #pragma push_macro/pop_macro later.
class MacroDirective {
 public:
  enum class Kind : uint8_t { kDefine, kUndefine };

  MacroDirective(Kind kind, MacroInfo* info, SourceLocation loc,
                 MacroDirective* previous)
      : previous_(previous), info_(info), loc_(loc), kind_(kind) {}

  Kind GetKind() const noexcept { return kind_; }
  bool IsDefinition() const noexcept { return kind_ == Kind::kDefine; }

  /// The defined macro, or nullptr for an #undef directive.
  MacroInfo* GetMacroInfo() const noexcept { return info_; }
  SourceLocation GetLocation() const noexcept { return loc_; }
  MacroDirective* GetPrevious() const noexcept { return previous_; }

 private:
  MacroDirective* previous_;
  MacroInfo* info_;
  SourceLocation loc_;
  Kind kind_;
};

}  // namespace bcc
