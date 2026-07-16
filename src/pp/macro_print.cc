#include "bcc/pp/macro_print.hh"

#include <string>
#include <string_view>

#include "bcc/common/string_util.hh"
#include "bcc/lex/token.hh"
#include "bcc/pp/identifier_table.hh"
#include "bcc/pp/macro_info.hh"

namespace bcc {

std::string FormatMacroDefine(const IdentifierInfo& name, const MacroInfo& macro) {
  std::string out = "#define ";
  out += name.GetName();

  if (macro.IsFunctionLike()) {
    out += '(';
    const std::vector<IdentifierInfo*>& params = macro.GetParameters();
    for (unsigned i = 0; i < params.size(); ++i) {
      if (i > 0) out += ", ";
      bool is_last = (i + 1 == params.size());
      if (macro.IsVariadic() && is_last) {
        // C99 `...` is stored as a trailing __VA_ARGS__ parameter; print it
        // back as `...`. GNU `name...` keeps the chosen name, printed as
        // `name...`.
        if (params[i]->GetName() == "__VA_ARGS__") {
          out += "...";
        } else {
          out += params[i]->GetName();
          out += "...";
        }
      } else {
        out += params[i]->GetName();
      }
    }
    out += ')';
  }

  // The replacement list is rejoined using each token's recorded leading-space
  // flag. The first body token's leading space is cleared at definition time
  // (it is not part of the macro), so a single space is emitted before any
  // non-empty body to separate it from the name / parameter list.
  bool first_body = true;
  for (const Token& t : macro.GetReplacementTokens()) {
    if (first_body) {
      out += ' ';
      first_body = false;
    } else if (t.HasLeadingSpace()) {
      out += ' ';
    }
    out += RemoveLineSplices(t.GetLexeme());
  }

  return out;
}

}  // namespace bcc
