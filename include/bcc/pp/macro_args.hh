#pragma once

#include <utility>
#include <vector>

#include "bcc/lex/token.hh"

namespace bcc {

class Preprocessor;

/// \brief The actual arguments supplied to one function-like macro invocation.
///
/// Holds one token list per formal parameter (including the trailing
/// __VA_ARGS__ list for variadic macros). Each argument's macro-expanded form
/// is computed lazily and cached, since only arguments that appear in the
/// replacement list — and are not operands of # or ## — need pre-expansion.
class MacroArgs {
 public:
  /// \param unexpanded_args One entry per formal parameter, in order.
  explicit MacroArgs(std::vector<std::vector<Token>> unexpanded_args)
      : unexpanded_args_(std::move(unexpanded_args)),
        expanded_args_(unexpanded_args_.size()),
        expanded_computed_(unexpanded_args_.size(), false) {}

  unsigned GetNumArgs() const noexcept {
    return static_cast<unsigned>(unexpanded_args_.size());
  }

  /// \brief The raw (unexpanded) tokens of argument \p i.
  const std::vector<Token>& GetUnexpArgument(unsigned i) const {
    return unexpanded_args_[i];
  }

  /// \brief The macro-expanded tokens of argument \p i, computed on first use.
  const std::vector<Token>& GetExpandedArgument(unsigned i, Preprocessor& pp);

 private:
  std::vector<std::vector<Token>> unexpanded_args_;
  std::vector<std::vector<Token>> expanded_args_;
  std::vector<bool> expanded_computed_;
};

}  // namespace bcc
