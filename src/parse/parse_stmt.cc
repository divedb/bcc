#include "bcc/parse/parser.hh"
#include "bcc/pp/identifier_table.hh"

namespace bcc {

//===----------------------------------------------------------------------===//
// Statement dispatch (Clang: ParseStatementOrDeclaration).
//===----------------------------------------------------------------------===//

StmtResult Parser::ParseStatement() {
  switch (tok_.GetKind()) {
    case TokenKind::kLBrace:
      return ParseCompoundStatement(Scope::kBlock | Scope::kDecl);
    case TokenKind::kSemi:
      return sema_.ActOnNullStmt(ConsumeToken());
    case TokenKind::kIf:
      return ParseIfStatement();
    case TokenKind::kSwitch:
      return ParseSwitchStatement();
    case TokenKind::kWhile:
      return ParseWhileStatement();
    case TokenKind::kDo:
      return ParseDoStatement();
    case TokenKind::kFor:
      return ParseForStatement();
    case TokenKind::kGoto:
      return ParseGotoStatement();
    case TokenKind::kContinue: {
      SourceLocation loc = ConsumeToken();
      StmtResult r = sema_.ActOnContinueStmt(loc, sema_.GetCurScope());
      ExpectAndConsumeSemi(diag::err_expected_semi_after_stmt, "continue");
      return r;
    }
    case TokenKind::kBreak: {
      SourceLocation loc = ConsumeToken();
      StmtResult r = sema_.ActOnBreakStmt(loc, sema_.GetCurScope());
      ExpectAndConsumeSemi(diag::err_expected_semi_after_stmt, "break");
      return r;
    }
    case TokenKind::kReturn:
      return ParseReturnStatement();
    case TokenKind::kCase:
      return ParseCaseStatement();
    case TokenKind::kDefault:
      return ParseDefaultStatement();
    case TokenKind::kIdentifier:
      if (NextToken().GetKind() == TokenKind::kColon) {
        return ParseLabeledStatement();
      }
      break;
    case TokenKind::kStaticAssert: {
      SourceLocation begin = tok_.GetLocation();
      SourceLocation end;
      std::vector<Decl*> decls = ParseSimpleDeclaration(
          DeclaratorContext::kBlock, &end);
      return sema_.ActOnDeclStmt(std::move(decls), {begin, end});
    }
    default:
      break;
  }

  if (IsDeclarationSpecifier()) {
    SourceLocation begin = tok_.GetLocation();
    SourceLocation end;
    std::vector<Decl*> decls =
        ParseSimpleDeclaration(DeclaratorContext::kBlock, &end);
    return sema_.ActOnDeclStmt(std::move(decls), {begin, end});
  }

  return ParseExprStatement();
}

StmtResult Parser::ParseCompoundStatement(unsigned scope_flags) {
  BalancedDelimiterTracker braces(*this, TokenKind::kLBrace);
  if (braces.ConsumeOpen()) {
    Diag(tok_, diag::err_expected) << "'{'";
    return StmtError();
  }

  ParseScope scope(this, scope_flags | Scope::kDecl);

  std::vector<Stmt*> stmts;
  while (!TokIs(TokenKind::kRBrace) && !TokIs(TokenKind::kEOF)) {
    SourceLocation before = tok_.GetLocation();
    StmtResult r = ParseStatement();
    if (r.IsInvalid()) {
      // Statement parsers recover on their own; only force progress here if
      // the parser is stuck on the same token.
      if (tok_.GetLocation() == before) {
        SkipUntil({TokenKind::kRBrace}, kStopBeforeMatch | kStopAtSemi);
        TryConsumeToken(TokenKind::kSemi);
      }
      continue;
    }
    if (r.IsUsable()) stmts.push_back(r.Get());
  }

  braces.ConsumeClose();
  return sema_.ActOnCompoundStmt(
      std::move(stmts),
      {braces.GetOpenLocation(), braces.GetCloseLocation()});
}

//===----------------------------------------------------------------------===//
// Selection statements.
//===----------------------------------------------------------------------===//

ExprResult Parser::ParseParenExprOrCondition(SourceLocation* rparen_loc) {
  BalancedDelimiterTracker parens(*this, TokenKind::kLParen);
  if (parens.ConsumeOpen()) {
    Diag(tok_, diag::err_expected) << "'('";
    return ExprResult::MakeInvalid();
  }
  ExprResult cond = ParseExpression();
  if (cond.IsInvalid()) {
    SkipUntil({TokenKind::kRParen}, kStopBeforeMatch | kStopAtSemi);
  }
  parens.ConsumeClose();
  if (rparen_loc) *rparen_loc = parens.GetCloseLocation();
  return cond;
}

StmtResult Parser::ParseIfStatement() {
  SourceLocation if_loc = ConsumeToken();

  ExprResult cond = ParseParenExprOrCondition();

  ParseScope then_scope(this, Scope::kDecl | Scope::kControl);
  StmtResult then_stmt = ParseStatement();

  StmtResult else_stmt;
  if (TryConsumeToken(TokenKind::kElse)) {
    else_stmt = ParseStatement();
  }

  if (cond.IsInvalid() || then_stmt.IsInvalid()) return StmtError();
  return sema_.ActOnIfStmt(if_loc, cond.Get(), then_stmt.Get(),
                           else_stmt.IsUsable() ? else_stmt.Get() : nullptr);
}

StmtResult Parser::ParseSwitchStatement() {
  SourceLocation switch_loc = ConsumeToken();

  ExprResult cond = ParseParenExprOrCondition();
  if (cond.IsInvalid()) {
    // Still parse the body for further diagnostics.
    ParseScope body_scope(this,
                          Scope::kBreak | Scope::kSwitch | Scope::kDecl);
    ParseStatement();
    return StmtError();
  }

  StmtResult sw = sema_.ActOnStartOfSwitchStmt(switch_loc, cond.Get());

  ParseScope body_scope(this, Scope::kBreak | Scope::kSwitch | Scope::kDecl);
  StmtResult body = ParseStatement();

  if (sw.IsInvalid()) return StmtError();
  return sema_.ActOnFinishSwitchStmt(sw.Get(),
                                     body.IsUsable() ? body.Get() : nullptr);
}

//===----------------------------------------------------------------------===//
// Iteration statements.
//===----------------------------------------------------------------------===//

StmtResult Parser::ParseWhileStatement() {
  SourceLocation while_loc = ConsumeToken();

  ExprResult cond = ParseParenExprOrCondition();

  ParseScope body_scope(this,
                        Scope::kBreak | Scope::kContinue | Scope::kDecl);
  StmtResult body = ParseStatement();

  if (cond.IsInvalid() || body.IsInvalid()) return StmtError();
  return sema_.ActOnWhileStmt(while_loc, cond.Get(), body.Get());
}

StmtResult Parser::ParseDoStatement() {
  SourceLocation do_loc = ConsumeToken();

  StmtResult body;
  {
    ParseScope body_scope(this,
                          Scope::kBreak | Scope::kContinue | Scope::kDecl);
    body = ParseStatement();
  }

  if (ExpectAndConsume(TokenKind::kWhile, diag::err_expected_after, "'do'")) {
    return StmtError();
  }

  SourceLocation rparen_loc;
  ExprResult cond = ParseParenExprOrCondition(&rparen_loc);
  ExpectAndConsumeSemi(diag::err_expected_semi_after_stmt, "do/while");

  if (cond.IsInvalid() || body.IsInvalid()) return StmtError();
  return sema_.ActOnDoStmt(do_loc, body.Get(), cond.Get(), rparen_loc);
}

StmtResult Parser::ParseForStatement() {
  SourceLocation for_loc = ConsumeToken();

  BalancedDelimiterTracker parens(*this, TokenKind::kLParen);
  if (parens.ConsumeOpen()) {
    Diag(tok_, diag::err_expected) << "'('";
    return StmtError();
  }

  // The init clause's declarations live in their own scope enclosing the
  // body (C11 6.8.5.3).
  ParseScope for_scope(this, Scope::kDecl | Scope::kControl);

  Stmt* init = nullptr;
  if (TokIs(TokenKind::kSemi)) {
    ConsumeToken();
  } else if (IsDeclarationSpecifier()) {
    SourceLocation begin = tok_.GetLocation();
    SourceLocation end;
    std::vector<Decl*> decls =
        ParseSimpleDeclaration(DeclaratorContext::kForInit, &end);
    StmtResult ds = sema_.ActOnDeclStmt(std::move(decls), {begin, end});
    if (ds.IsUsable()) init = ds.Get();
  } else {
    ExprResult init_expr = ParseExpression();
    if (init_expr.IsUsable()) init = init_expr.Get();
    if (ExpectAndConsume(TokenKind::kSemi, diag::err_expected_semi_after_expr)) {
      SkipUntil({TokenKind::kSemi});
    }
  }

  ExprResult cond;
  if (!TokIs(TokenKind::kSemi)) cond = ParseExpression();
  ExpectAndConsume(TokenKind::kSemi, diag::err_expected_semi_after_expr);

  ExprResult inc;
  if (!TokIs(TokenKind::kRParen)) inc = ParseExpression();
  parens.ConsumeClose();

  StmtResult body;
  {
    ParseScope body_scope(this,
                          Scope::kBreak | Scope::kContinue | Scope::kDecl);
    body = ParseStatement();
  }

  if (body.IsInvalid() || cond.IsInvalid() || inc.IsInvalid()) {
    return StmtError();
  }
  return sema_.ActOnForStmt(for_loc, init,
                            cond.IsUsable() ? cond.Get()->As<Expr>() : nullptr,
                            inc.IsUsable() ? inc.Get()->As<Expr>() : nullptr,
                            body.Get());
}

//===----------------------------------------------------------------------===//
// Jump statements.
//===----------------------------------------------------------------------===//

StmtResult Parser::ParseReturnStatement() {
  SourceLocation return_loc = ConsumeToken();

  ExprResult value;
  if (!TokIs(TokenKind::kSemi)) {
    value = ParseExpression();
    if (value.IsInvalid()) {
      SkipUntil({TokenKind::kSemi}, kStopBeforeMatch | kStopAtSemi);
      TryConsumeToken(TokenKind::kSemi);
      return StmtError();
    }
  }
  ExpectAndConsumeSemi(diag::err_expected_semi_after_stmt, "return");
  return sema_.ActOnReturnStmt(return_loc,
                               value.IsUsable() ? value.Get() : nullptr);
}

StmtResult Parser::ParseGotoStatement() {
  SourceLocation goto_loc = ConsumeToken();

  if (!TokIs(TokenKind::kIdentifier)) {
    Diag(tok_, diag::err_expected_ident);
    SkipUntil({TokenKind::kSemi});
    return StmtError();
  }
  const IdentifierInfo* label = tok_.GetIdentifierInfo();
  SourceLocation label_loc = ConsumeToken();
  ExpectAndConsumeSemi(diag::err_expected_semi_after_stmt, "goto");
  return sema_.ActOnGotoStmt(goto_loc, label, label_loc);
}

//===----------------------------------------------------------------------===//
// Labeled statements.
//===----------------------------------------------------------------------===//

StmtResult Parser::ParseCaseStatement() {
  SourceLocation case_loc = ConsumeToken();

  ExprResult value = ParseConstantExpression();
  SourceLocation colon_loc = tok_.GetLocation();
  if (ExpectAndConsume(TokenKind::kColon, diag::err_expected_case_colon,
                       "case")) {
    return StmtError();
  }

  StmtResult cs;
  if (value.IsUsable()) {
    cs = sema_.ActOnCaseStmt(case_loc, value.Get(), colon_loc);
  }

  // `case 1: }` — a label with no statement is invalid.
  if (TokIs(TokenKind::kRBrace)) {
    Diag(tok_, diag::err_expected_statement);
    return StmtError();
  }

  StmtResult sub = ParseStatement();
  if (cs.IsUsable()) {
    sema_.ActOnCaseStmtBody(cs.Get(), sub.IsUsable() ? sub.Get() : nullptr);
    return cs;
  }
  return sub;
}

StmtResult Parser::ParseDefaultStatement() {
  SourceLocation default_loc = ConsumeToken();

  if (ExpectAndConsume(TokenKind::kColon, diag::err_expected_case_colon,
                       "default")) {
    return StmtError();
  }

  StmtResult ds = sema_.ActOnDefaultStmt(default_loc, nullptr);

  if (TokIs(TokenKind::kRBrace)) {
    Diag(tok_, diag::err_expected_statement);
    return StmtError();
  }

  StmtResult sub = ParseStatement();
  if (ds.IsUsable()) {
    if (auto* stmt = ds.Get()->As<DefaultStmt>()) {
      stmt->SetSubStmt(sub.IsUsable() ? sub.Get() : nullptr);
    }
    return ds;
  }
  return sub;
}

StmtResult Parser::ParseLabeledStatement() {
  const IdentifierInfo* label = tok_.GetIdentifierInfo();
  SourceLocation label_loc = ConsumeToken();
  ConsumeToken();  // ':'

  if (TokIs(TokenKind::kRBrace)) {
    Diag(tok_, diag::err_expected_statement);
    return StmtError();
  }

  StmtResult sub = ParseStatement();
  return sema_.ActOnLabelStmt(label_loc, label,
                              sub.IsUsable() ? sub.Get() : nullptr);
}

//===----------------------------------------------------------------------===//
// Expression statements.
//===----------------------------------------------------------------------===//

StmtResult Parser::ParseExprStatement() {
  ExprResult expr = ParseExpression();
  if (expr.IsInvalid()) {
    SkipUntil({TokenKind::kRBrace}, kStopBeforeMatch | kStopAtSemi);
    TryConsumeToken(TokenKind::kSemi);
    return StmtError();
  }
  ExpectAndConsumeSemi(diag::err_expected_semi_after_expr);
  return sema_.ActOnExprStmt(expr.Get());
}

}  // namespace bcc
