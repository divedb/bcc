#include "bcc/parse/parser.hh"
#include "bcc/pp/identifier_table.hh"

namespace bcc {

ExprResult Parser::ParseInitializer() {
  if (TokIs(TokenKind::kLBrace)) return ParseBraceInitializer();
  return ParseAssignmentExpression();
}

ExprResult Parser::ParseBraceInitializer() {
  BalancedDelimiterTracker braces(*this, TokenKind::kLBrace);
  braces.ConsumeOpen();

  std::vector<Expr*> inits;
  bool had_error = false;

  if (!TokIs(TokenKind::kRBrace)) {
    for (;;) {
      ExprResult init = ParseInitializerWithPotentialDesignator();
      if (init.IsUsable()) {
        inits.push_back(init.Get());
      } else {
        had_error = true;
        // Recover at the next ',' or '}'.
        if (!SkipUntil({TokenKind::kComma, TokenKind::kRBrace},
                       kStopBeforeMatch)) {
          break;
        }
      }
      if (!TryConsumeToken(TokenKind::kComma)) break;
      if (TokIs(TokenKind::kRBrace)) break;  // trailing comma
    }
  }

  braces.ConsumeClose();
  if (had_error) return ExprResult::MakeInvalid();
  return sema_.ActOnInitList(braces.GetOpenLocation(), std::move(inits),
                             braces.GetCloseLocation());
}

ExprResult Parser::ParseInitializerWithPotentialDesignator() {
  if (!TokIs(TokenKind::kPeriod) && !TokIs(TokenKind::kLSquare)) {
    return ParseInitializer();
  }

  std::vector<Designator> designators;
  for (;;) {
    if (TokIs(TokenKind::kPeriod)) {
      SourceLocation dot_loc = ConsumeToken();
      if (!TokIs(TokenKind::kIdentifier)) {
        Diag(tok_, diag::err_designator_expected_field_name);
        return ExprResult::MakeInvalid();
      }
      Designator d;
      d.field = tok_.GetIdentifierInfo();
      d.loc = dot_loc;
      ConsumeToken();
      designators.push_back(d);
    } else if (TokIs(TokenKind::kLSquare)) {
      BalancedDelimiterTracker brackets(*this, TokenKind::kLSquare);
      brackets.ConsumeOpen();
      ExprResult index = ParseConstantExpression();
      brackets.ConsumeClose();
      if (index.IsInvalid()) return ExprResult::MakeInvalid();
      Designator d;
      d.index = index.Get();
      d.loc = brackets.GetOpenLocation();
      designators.push_back(d);
    } else {
      break;
    }
  }

  if (ExpectAndConsume(TokenKind::kEqual, diag::err_expected_equal_designator)) {
    return ExprResult::MakeInvalid();
  }

  ExprResult init = ParseInitializer();
  if (init.IsInvalid()) return init;
  return sema_.ActOnDesignatedInit(std::move(designators), init.Get());
}

}  // namespace bcc
