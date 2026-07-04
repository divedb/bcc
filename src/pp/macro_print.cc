#include "bcc/pp/macro_print.hh"

#include <string>
#include <string_view>

#include "bcc/common/string_util.hh"
#include "bcc/lex/token.hh"
#include "bcc/pp/identifier_table.hh"
#include "bcc/pp/macro_info.hh"

namespace bcc {

namespace {

/// Strips backslash-newline line splices from a raw lexeme so a macro body that
/// spans a splice (e.g. `#define X fo\\\no`) rejoins to its logical spelling.
std::string CleanLexeme(std::string_view raw) {
  if (raw.find('\\') == std::string_view::npos) return std::string(raw);
  std::string out;
  out.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size();) {
    if (raw[i] == '\\' && i + 1 < raw.size() &&
        IsNewLine(static_cast<unsigned char>(raw[i + 1]))) {
      char newline = raw[i + 1];
      i += 2;
      if (newline == '\r' && i < raw.size() && raw[i] == '\n') ++i;
      continue;
    }
    out.push_back(raw[i++]);
  }
  return out;
}

}  // namespace

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
    out += CleanLexeme(t.GetLexeme());
  }

  return out;
}

}  // namespace bcc
