#include "bcc/parse/parser.hh"

#include "bcc/ast/ast_context.hh"

namespace bcc {

Parser::Parser(Preprocessor& pp, Sema& sema)
    : pp_(pp),
      sema_(sema),
      tok_(SourceLocation{}, TokenKind::kUnknown, nullptr, 0) {
  // Prime the first token. The caller must have entered the main file.
  tok_ = pp_.Lex();
}

SourceLocation Parser::ConsumeToken() {
  SourceLocation loc = tok_.GetLocation();
  tok_ = pp_.Lex();
  return loc;
}

bool Parser::TryConsumeToken(TokenKind k) {
  if (!TokIs(k)) return false;
  ConsumeToken();
  return true;
}

bool Parser::ExpectAndConsume(TokenKind k, diag::DiagKind diag_kind,
                              std::string_view arg) {
  if (TryConsumeToken(k)) return false;
  DiagnosticBuilder builder = Diag(tok_, diag_kind);
  if (!arg.empty()) builder << arg;
  return true;
}

bool Parser::ExpectAndConsumeSemi(diag::DiagKind diag_kind,
                                  std::string_view arg) {
  if (ExpectAndConsume(TokenKind::kSemi, diag_kind, arg)) {
    // Recover: skip to the next ';' (or a safe stopping point) and eat it so
    // parsing can resume at the following construct.
    SkipUntil({TokenKind::kSemi});
    return true;
  }
  return false;
}

bool Parser::SkipUntil(std::initializer_list<TokenKind> kinds,
                       unsigned flags) {
  for (;;) {
    for (TokenKind k : kinds) {
      if (TokIs(k)) {
        if (!(flags & kStopBeforeMatch)) ConsumeToken();
        return true;
      }
    }

    switch (tok_.GetKind()) {
      case TokenKind::kEOF:
        return false;
      case TokenKind::kLParen:
        ConsumeToken();
        SkipUntil({TokenKind::kRParen});
        break;
      case TokenKind::kLSquare:
        ConsumeToken();
        SkipUntil({TokenKind::kRSquare});
        break;
      case TokenKind::kLBrace:
        ConsumeToken();
        SkipUntil({TokenKind::kRBrace});
        break;
      // An unmatched closer belongs to an outer construct: stop here.
      case TokenKind::kRParen:
      case TokenKind::kRSquare:
      case TokenKind::kRBrace:
        return false;
      case TokenKind::kSemi:
        if (flags & kStopAtSemi) return false;
        ConsumeToken();
        break;
      default:
        ConsumeToken();
        break;
    }
  }
}

bool Parser::BalancedDelimiterTracker::ConsumeOpen() {
  if (!parser_.TokIs(open_kind_)) return true;
  open_loc_ = parser_.ConsumeToken();
  return false;
}

bool Parser::BalancedDelimiterTracker::ConsumeClose() {
  TokenKind close_kind;
  diag::DiagKind expected_diag;
  diag::DiagKind note_diag;
  switch (open_kind_) {
    case TokenKind::kLParen:
      close_kind = TokenKind::kRParen;
      expected_diag = diag::err_expected_rparen;
      note_diag = diag::note_to_match_this_lparen;
      break;
    case TokenKind::kLSquare:
      close_kind = TokenKind::kRSquare;
      expected_diag = diag::err_expected_rsquare;
      note_diag = diag::note_to_match_this_lsquare;
      break;
    default:
      close_kind = TokenKind::kRBrace;
      expected_diag = diag::err_expected_rbrace;
      note_diag = diag::note_to_match_this_lbrace;
      break;
  }

  if (parser_.TokIs(close_kind)) {
    close_loc_ = parser_.ConsumeToken();
    return false;
  }

  parser_.Diag(parser_.Tok(), expected_diag);
  parser_.Diag(open_loc_, note_diag);
  // Recover: skip to the matching closer if it is findable.
  if (parser_.SkipUntil({close_kind}, kStopBeforeMatch)) {
    close_loc_ = parser_.ConsumeToken();
  }
  return true;
}

void Parser::ParseTranslationUnit() {
  // The translation-unit (file) scope.
  ParseScope tu_scope(this, Scope::kDecl);

  while (!TokIs(TokenKind::kEOF)) {
    ParseExternalDeclaration();
  }

  sema_.ActOnEndOfTranslationUnit();
}

}  // namespace bcc
