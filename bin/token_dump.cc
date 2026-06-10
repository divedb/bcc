#include <iostream>
#include <string>

#include "bcc/lex/lexer.hh"
#include "bcc/lex/source_buffer.hh"
#include "bcc/lex/token.hh"
#include "bcc/lex/token_kind.hh"

int main() {
  std::string source;
  std::string line;

  while (std::getline(std::cin, line)) {
    source += line;
    source += '\n';
  }

  bcc::SourceBuffer buffer(source);
  bcc::BufferedLexer lexer(buffer);

  for (;;) {
    bcc::Token token = lexer.NextToken();
    int offset = token.GetLocation().offset;
    std::string_view spelling = token.GetLexeme();
    std::string_view kind_name = bcc::TokenKindName(token.GetKind());

    std::cout << offset << " | " << kind_name;
    if (token.NeedsCleaning()) std::cout << " [needs_cleaning]";
    if (token.IsStartOfLine()) std::cout << " [start_of_line]";
    if (token.HasLeadingSpace()) std::cout << " [leading_space]";
    std::cout << " | " << spelling << "\n";

    if (token.GetKind() == bcc::TokenKind::kEOF) break;
  }

  return 0;
}
