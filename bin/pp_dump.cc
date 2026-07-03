// pp_dump — a minimal `-E` driver mirroring token_dump.
//
// Reads a translation unit, runs it through the Preprocessor, and prints the
// fully preprocessed text (macros expanded, directives consumed, includes
// spliced in), with GNU-style line markers so the output tracks presumed
// locations. `-I<dir>` adds an angled-include search path.

#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>

#include "bcc/basic/diagnostic.hh"
#include "bcc/basic/file_manager.hh"
#include "bcc/basic/presumed_loc.hh"
#include "bcc/basic/source_manager.hh"
#include "bcc/basic/text_diagnostic_printer.hh"
#include "bcc/pp/header_search.hh"
#include "bcc/pp/preprocessor.hh"
#include "bcc/lex/token.hh"
#include "bcc/lex/token_kind.hh"

namespace {

// Removes backslash-newline line splices from a raw lexeme so the emitted text
// is the token's logical spelling.
std::string CleanLexeme(std::string_view raw) {
  if (raw.find('\\') == std::string_view::npos) return std::string(raw);
  std::string out;
  out.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size();) {
    if (raw[i] == '\\' && i + 1 < raw.size() &&
        (raw[i + 1] == '\n' || raw[i + 1] == '\r')) {
      char nl = raw[i + 1];
      i += 2;
      if (nl == '\r' && i < raw.size() && raw[i] == '\n') ++i;
      continue;
    }
    out += raw[i++];
  }
  return out;
}

/// Reconstructs `-E` text from the preprocessed token stream, using presumed
/// locations to place newlines and line markers.
class TextPrinter {
 public:
  TextPrinter(bcc::SourceManager& sm, std::ostream& os) : sm_(sm), os_(os) {}

  void Emit(const bcc::Token& tok) {
    // Attribute macro-expanded tokens to the invocation site, not the macro
    // body, so the emitted text tracks where the code was written.
    bcc::PresumedLoc p =
        sm_.GetPresumedLoc(sm_.GetExpansionLoc(tok.GetLocation()));
    unsigned line = p.IsValid() ? p.line : line_;
    std::string_view file = p.IsValid() ? p.filename : file_;

    if (first_ || file != file_) {
      if (!first_) os_ << '\n';
      LineMarker(line, file);
    } else if (line > line_) {
      // A small forward gap is printed as blank lines; a large one as a marker.
      if (line - line_ <= 8) {
        for (unsigned i = line_; i < line; ++i) os_ << '\n';
      } else {
        os_ << '\n';
        LineMarker(line, file);
      }
    } else if (line < line_) {
      os_ << '\n';
      LineMarker(line, file);
    } else if (tok.HasLeadingSpace()) {
      os_ << ' ';
    }

    os_ << CleanLexeme(tok.GetLexeme());
    line_ = line;
    file_ = file;
    first_ = false;
    at_line_start_ = false;
  }

  void Finish() {
    if (!first_) os_ << '\n';
  }

 private:
  void LineMarker(unsigned line, std::string_view file) {
    os_ << "# " << line << " \"" << file << "\"\n";
    line_ = line;
    file_ = std::string(file);
    at_line_start_ = true;
  }

  bcc::SourceManager& sm_;
  std::ostream& os_;
  unsigned line_ = 0;
  std::string file_;
  bool first_ = true;
  bool at_line_start_ = true;
};

}  // namespace

int main(int argc, char* argv[]) {
  bcc::FileManager fm;
  bcc::SourceManager sm(fm);
  bcc::TextDiagnosticPrinter printer(std::cerr);
  bcc::DiagnosticsEngine diag(&printer, &sm);
  bcc::HeaderSearch header_search(fm);

  const char* input_path = nullptr;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg.rfind("-I", 0) == 0) {
      header_search.AddAngledSearchPath(std::string(arg.substr(2)));
    } else {
      input_path = argv[i];
    }
  }

  const bcc::FileEntry* entry =
      input_path ? fm.GetFile(input_path) : fm.GetStdin();
  if (!entry) {
    diag.Report(bcc::diag::err_no_input_file);
    return 1;
  }

  sm.SetMainFileID(sm.CreateFileID(*entry));

  bcc::Preprocessor pp(sm, diag);
  pp.SetHeaderSearch(header_search);
  pp.EnterMainFile();

  TextPrinter out(sm, std::cout);
  for (;;) {
    bcc::Token tok = pp.Lex();
    if (tok.GetKind() == bcc::TokenKind::kEOF) break;
    out.Emit(tok);
  }
  out.Finish();

  return diag.HasErrors() ? 1 : 0;
}
