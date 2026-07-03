#include "bcc/pp/pp_lexer.hh"

namespace bcc {

PPLexer::PPLexer(SourceManager& sm, FileID fid, DiagnosticsEngine* diag,
                 const FileEntry* fe)
    : lexer_(sm, fid, diag), fid_(fid), file_entry_(fe) {}

Token PPLexer::MakeEodToken(const Token& trivia) noexcept {
  // A zero-length marker at the terminating newline (or EOF). Its flags are
  // irrelevant to end-of-directive handling, so they are left cleared.
  return Token{trivia.GetLocation(), TokenKind::kEod, trivia.GetLexeme().data(),
               0u};
}

Token PPLexer::Lex() {
  for (;;) {
    Token token = lexer_.NextToken();

    switch (token.GetKind()) {
      case TokenKind::kWhitespace:
      case TokenKind::kComment:
        // Trivia: drop it. The following token already carries the resulting
        // kLeadingSpace flag from the underlying lexer.
        continue;

      case TokenKind::kNewLine:
        if (parsing_preprocessor_directive_) {
          parsing_preprocessor_directive_ = false;
          return MakeEodToken(token);
        }
        continue;  // Trivia outside a directive.

      case TokenKind::kEOF:
        if (parsing_preprocessor_directive_) {
          // A directive with no terminating newline (end of file). Synthesize
          // the end-of-directive marker; the next Lex() returns the EOF token,
          // which the underlying lexer produces idempotently.
          parsing_preprocessor_directive_ = false;
          return MakeEodToken(token);
        }
        return token;

      default:
        return token;
    }
  }
}

Token PPLexer::LexIncludeFilename() {
  Token token = lexer_.LexIncludeFilename();

  // If the filename was missing and the directive line ended, surface kEod
  // exactly as Lex() would have.
  if (parsing_preprocessor_directive_ &&
      (token.GetKind() == TokenKind::kNewLine ||
       token.GetKind() == TokenKind::kEOF)) {
    parsing_preprocessor_directive_ = false;
    return MakeEodToken(token);
  }

  return token;
}

}  // namespace bcc
