#include <iostream>
#include <string>
#include <string_view>

#include "bcc/basic/file_manager.hh"
#include "bcc/basic/source_manager.hh"
#include "bcc/lex/lexer.hh"

namespace {

std::string EscapeSpelling(std::string_view s) {
  std::string out;
  out.reserve(s.size());

  for (unsigned char c : s) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;

      case '\'':
        out += "\\'";
        break;

      case '\n':
        out += "\\n";
        break;

      case '\r':
        out += "\\r";
        break;

      case '\t':
        out += "\\t";
        break;

      default:
        if (c < 0x20 || c == 0x7f) {
          // Control characters as \xHH
          const char hex[] = "0123456789abcdef";
          out += "\\x";
          out += hex[c >> 4];
          out += hex[c & 0xf];
        } else {
          out += static_cast<char>(c);
        }
    }
  }

  return out;
}

// Reads source from argv[1] if provided, otherwise from stdin.
// Returns nullopt and prints an error if the file cannot be opened.
bcc::FileID GetFileID(int argc, char* argv[], bcc::FileManager& fm,
                      bcc::SourceManager& sm) {
  const bcc::FileEntry* entry = argc > 1 ? fm.GetFile(argv[1]) : fm.GetStdin();

  if (!entry) {
    std::cerr << "Error: Unable to open file '"
              << (argc > 1 ? argv[1] : "<stdin>") << "'\n";

    return bcc::FileID{};
  }

  return sm.CreateFileID(*entry);
}

}  // namespace

int main(int argc, char* argv[]) {
  bcc::FileManager fm;
  bcc::SourceManager sm(fm);
  bcc::FileID fid = GetFileID(argc, argv, fm, sm);

  if (!fid.IsValid()) return 1;

  bcc::BufferedLexer lexer(sm, fid);

  for (;;) {
    bcc::Token tok = lexer.NextToken();
    bcc::SourceLocation loc = tok.GetLocation();

    std::cout << bcc::TokenKindName(tok.GetKind()) << " '"
              << EscapeSpelling(tok.GetLexeme()) << '\'';

    if (tok.IsStartOfLine()) std::cout << "\t [StartOfLine]";
    if (tok.HasLeadingSpace()) std::cout << "\t [LeadingSpace]";

    auto sloc = sm.GetSpellingLoc(loc);
    std::cout << "\tLoc=<" << sm.GetFilename(fid) << ':'
              << sm.GetLine(sloc) << ':' << sm.GetColumn(sloc) << ">\n";

    if (tok.GetKind() == bcc::TokenKind::kEOF) break;
  }

  return 0;
}
