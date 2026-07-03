#include "bcc/pp/macro_args.hh"

#include "bcc/pp/preprocessor.hh"

namespace bcc {

const std::vector<Token>& MacroArgs::GetExpandedArgument(unsigned i,
                                                         Preprocessor& pp) {
  if (!expanded_computed_[i]) {
    expanded_args_[i] = pp.ExpandArgument(unexpanded_args_[i]);
    expanded_computed_[i] = true;
  }
  return expanded_args_[i];
}

}  // namespace bcc
