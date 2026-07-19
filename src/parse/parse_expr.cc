#include "bcc/parse/parser.hh"
#include "bcc/pp/identifier_table.hh"

namespace bcc {

namespace {

/// Binary operator precedence levels, mirroring Clang's prec::Level.
enum Prec {
  kPrecUnknown = 0,
  kPrecComma = 1,
  kPrecAssignment = 2,
  kPrecConditional = 3,
  kPrecLogicalOr = 4,
  kPrecLogicalAnd = 5,
  kPrecInclusiveOr = 6,
  kPrecExclusiveOr = 7,
  kPrecAnd = 8,
  kPrecEquality = 9,
  kPrecRelational = 10,
  kPrecShift = 11,
  kPrecAdditive = 12,
  kPrecMultiplicative = 13,
};

int GetBinOpPrecedence(TokenKind kind) {
  switch (kind) {
    case TokenKind::kComma: return kPrecComma;
    case TokenKind::kEqual:
    case TokenKind::kStarEqual:
    case TokenKind::kSlashEqual:
    case TokenKind::kPercentEqual:
    case TokenKind::kPlusEqual:
    case TokenKind::kMinusEqual:
    case TokenKind::kLessLessEqual:
    case TokenKind::kGreaterGreaterEqual:
    case TokenKind::kAmpEqual:
    case TokenKind::kCaretEqual:
    case TokenKind::kPipeEqual:
      return kPrecAssignment;
    case TokenKind::kQuestion: return kPrecConditional;
    case TokenKind::kPipePipe: return kPrecLogicalOr;
    case TokenKind::kAmpAmp: return kPrecLogicalAnd;
    case TokenKind::kPipe: return kPrecInclusiveOr;
    case TokenKind::kCaret: return kPrecExclusiveOr;
    case TokenKind::kAmp: return kPrecAnd;
    case TokenKind::kEqualEqual:
    case TokenKind::kExclaimEqual:
      return kPrecEquality;
    case TokenKind::kLess:
    case TokenKind::kGreater:
    case TokenKind::kLessEqual:
    case TokenKind::kGreaterEqual:
      return kPrecRelational;
    case TokenKind::kLessLess:
    case TokenKind::kGreaterGreater:
      return kPrecShift;
    case TokenKind::kPlus:
    case TokenKind::kMinus:
      return kPrecAdditive;
    case TokenKind::kStar:
    case TokenKind::kSlash:
    case TokenKind::kPercent:
      return kPrecMultiplicative;
    default:
      return kPrecUnknown;
  }
}

BinaryOperatorKind ConvertTokenToBinaryOp(TokenKind kind) {
  switch (kind) {
    case TokenKind::kStar: return BinaryOperatorKind::kMul;
    case TokenKind::kSlash: return BinaryOperatorKind::kDiv;
    case TokenKind::kPercent: return BinaryOperatorKind::kRem;
    case TokenKind::kPlus: return BinaryOperatorKind::kAdd;
    case TokenKind::kMinus: return BinaryOperatorKind::kSub;
    case TokenKind::kLessLess: return BinaryOperatorKind::kShl;
    case TokenKind::kGreaterGreater: return BinaryOperatorKind::kShr;
    case TokenKind::kLess: return BinaryOperatorKind::kLT;
    case TokenKind::kGreater: return BinaryOperatorKind::kGT;
    case TokenKind::kLessEqual: return BinaryOperatorKind::kLE;
    case TokenKind::kGreaterEqual: return BinaryOperatorKind::kGE;
    case TokenKind::kEqualEqual: return BinaryOperatorKind::kEQ;
    case TokenKind::kExclaimEqual: return BinaryOperatorKind::kNE;
    case TokenKind::kAmp: return BinaryOperatorKind::kAnd;
    case TokenKind::kCaret: return BinaryOperatorKind::kXor;
    case TokenKind::kPipe: return BinaryOperatorKind::kOr;
    case TokenKind::kAmpAmp: return BinaryOperatorKind::kLAnd;
    case TokenKind::kPipePipe: return BinaryOperatorKind::kLOr;
    case TokenKind::kEqual: return BinaryOperatorKind::kAssign;
    case TokenKind::kStarEqual: return BinaryOperatorKind::kMulAssign;
    case TokenKind::kSlashEqual: return BinaryOperatorKind::kDivAssign;
    case TokenKind::kPercentEqual: return BinaryOperatorKind::kRemAssign;
    case TokenKind::kPlusEqual: return BinaryOperatorKind::kAddAssign;
    case TokenKind::kMinusEqual: return BinaryOperatorKind::kSubAssign;
    case TokenKind::kLessLessEqual: return BinaryOperatorKind::kShlAssign;
    case TokenKind::kGreaterGreaterEqual:
      return BinaryOperatorKind::kShrAssign;
    case TokenKind::kAmpEqual: return BinaryOperatorKind::kAndAssign;
    case TokenKind::kCaretEqual: return BinaryOperatorKind::kXorAssign;
    case TokenKind::kPipeEqual: return BinaryOperatorKind::kOrAssign;
    default: return BinaryOperatorKind::kComma;
  }
}

}  // namespace

//===----------------------------------------------------------------------===//
// Entry points.
//===----------------------------------------------------------------------===//

ExprResult Parser::ParseExpression() {
  ExprResult lhs = ParseAssignmentExpression();
  return ParseRHSOfBinaryExpression(lhs, kPrecComma);
}

ExprResult Parser::ParseAssignmentExpression() {
  ExprResult lhs = ParseCastExpression(/*is_unary_context=*/false);
  return ParseRHSOfBinaryExpression(lhs, kPrecAssignment);
}

ExprResult Parser::ParseConstantExpression() {
  ExprResult lhs = ParseCastExpression(/*is_unary_context=*/false);
  return ParseRHSOfBinaryExpression(lhs, kPrecConditional);
}

//===----------------------------------------------------------------------===//
// Precedence climbing (Clang: ParseRHSOfBinaryExpression).
//===----------------------------------------------------------------------===//

ExprResult Parser::ParseRHSOfBinaryExpression(ExprResult lhs, int min_prec) {
  for (;;) {
    int prec = GetBinOpPrecedence(tok_.GetKind());
    if (prec < min_prec) return lhs;

    Token op_tok = tok_;
    SourceLocation op_loc = ConsumeToken();

    // Ternary: lhs ? expr : conditional-expression.
    if (op_tok.GetKind() == TokenKind::kQuestion) {
      ExprResult middle = ParseExpression();
      SourceLocation colon_loc = tok_.GetLocation();
      if (ExpectAndConsume(TokenKind::kColon, diag::err_expected, "':'")) {
        return ExprResult::MakeInvalid();
      }
      ExprResult rhs = ParseAssignmentExpression();
      if (lhs.IsInvalid() || middle.IsInvalid() || rhs.IsInvalid()) {
        lhs = ExprResult::MakeInvalid();
      } else {
        lhs = sema_.ActOnConditionalOp(op_loc, colon_loc, lhs.Get(),
                                       middle.Get(), rhs.Get());
      }
      continue;
    }

    ExprResult rhs = ParseCastExpression(/*is_unary_context=*/false);

    // If the next operator binds tighter (or is right-associative at the
    // same level), it takes the RHS first.
    int next_prec = GetBinOpPrecedence(tok_.GetKind());
    bool right_assoc = prec == kPrecAssignment || prec == kPrecConditional;
    if (next_prec > prec || (right_assoc && next_prec == prec) ||
        (tok_.GetKind() == TokenKind::kQuestion && next_prec >= prec)) {
      rhs = ParseRHSOfBinaryExpression(rhs, prec + (right_assoc ? 0 : 1));
    }

    if (lhs.IsInvalid() || rhs.IsInvalid()) {
      lhs = ExprResult::MakeInvalid();
      continue;
    }
    lhs = sema_.ActOnBinOp(op_loc, ConvertTokenToBinaryOp(op_tok.GetKind()),
                           lhs.Get(), rhs.Get());
  }
}

//===----------------------------------------------------------------------===//
// Cast / unary / primary expressions (Clang: ParseCastExpression).
//===----------------------------------------------------------------------===//

ExprResult Parser::ParseCastExpression(bool is_unary_context) {
  ExprResult res;

  switch (tok_.GetKind()) {
    case TokenKind::kNumericConstant:
      res = sema_.ActOnNumericConstant(tok_);
      ConsumeToken();
      break;

    case TokenKind::kCharConstant:
    case TokenKind::kWideCharConstant:
    case TokenKind::kUtf16CharConstant:
    case TokenKind::kUtf32CharConstant:
      res = sema_.ActOnCharacterConstant(tok_);
      ConsumeToken();
      break;

    case TokenKind::kStringLiteral:
    case TokenKind::kUtf8StringLiteral:
    case TokenKind::kUtf16StringLiteral:
    case TokenKind::kUtf32StringLiteral:
    case TokenKind::kWideStringLiteral:
      res = ParseStringLiteralExpression();
      break;

    case TokenKind::kIdentifier: {
      const IdentifierInfo* name = tok_.GetIdentifierInfo();
      SourceLocation loc = ConsumeToken();
      bool is_callee = TokIs(TokenKind::kLParen);
      res = sema_.ActOnIdExpression(sema_.GetCurScope(), name, loc,
                                    is_callee);
      break;
    }

    case TokenKind::kLParen: {
      ParenParseOption opt = ParenParseOption::kExpression;
      QualType cast_type;
      SourceLocation rparen_loc;
      SourceLocation lparen_loc = tok_.GetLocation();
      res = ParseParenExpression(opt, cast_type, rparen_loc);
      if (opt == ParenParseOption::kCastExpr) {
        // `(T) cast-expression`
        ExprResult operand = ParseCastExpression(false);
        if (operand.IsInvalid() || cast_type.IsNull()) {
          return ExprResult::MakeInvalid();
        }
        return sema_.ActOnCastExpr(lparen_loc, cast_type, rparen_loc,
                                   operand.Get());
      }
      break;
    }

    case TokenKind::kPlusPlus:
    case TokenKind::kMinusMinus: {
      bool is_inc = TokIs(TokenKind::kPlusPlus);
      SourceLocation loc = ConsumeToken();
      ExprResult operand = ParseCastExpression(true);
      if (operand.IsInvalid()) return operand;
      return sema_.ActOnUnaryOp(loc,
                                is_inc ? UnaryOperatorKind::kPreInc
                                       : UnaryOperatorKind::kPreDec,
                                operand.Get());
    }

    case TokenKind::kAmp:
    case TokenKind::kStar:
    case TokenKind::kPlus:
    case TokenKind::kMinus:
    case TokenKind::kTilde:
    case TokenKind::kExclaim: {
      UnaryOperatorKind op;
      switch (tok_.GetKind()) {
        case TokenKind::kAmp: op = UnaryOperatorKind::kAddrOf; break;
        case TokenKind::kStar: op = UnaryOperatorKind::kDeref; break;
        case TokenKind::kPlus: op = UnaryOperatorKind::kPlus; break;
        case TokenKind::kMinus: op = UnaryOperatorKind::kMinus; break;
        case TokenKind::kTilde: op = UnaryOperatorKind::kNot; break;
        default: op = UnaryOperatorKind::kLNot; break;
      }
      SourceLocation loc = ConsumeToken();
      ExprResult operand = ParseCastExpression(false);
      if (operand.IsInvalid()) return operand;
      return sema_.ActOnUnaryOp(loc, op, operand.Get());
    }

    case TokenKind::kSizeof:
    case TokenKind::kAlignof:
      return ParseSizeofAlignofExpression();

    case TokenKind::kGeneric:
      res = ParseGenericSelectionExpression();
      break;

    default:
      Diag(tok_, diag::err_expected_expression);
      return ExprResult::MakeInvalid();
  }

  (void)is_unary_context;
  return ParsePostfixExpressionSuffix(res);
}

//===----------------------------------------------------------------------===//
// Postfix suffixes: call, index, member, post-inc/dec.
//===----------------------------------------------------------------------===//

ExprResult Parser::ParsePostfixExpressionSuffix(ExprResult lhs) {
  for (;;) {
    switch (tok_.GetKind()) {
      case TokenKind::kLSquare: {
        BalancedDelimiterTracker brackets(*this, TokenKind::kLSquare);
        brackets.ConsumeOpen();
        ExprResult idx = ParseExpression();
        brackets.ConsumeClose();
        if (lhs.IsInvalid() || idx.IsInvalid()) {
          lhs = ExprResult::MakeInvalid();
        } else {
          lhs = sema_.ActOnArraySubscript(lhs.Get(),
                                          brackets.GetOpenLocation(),
                                          idx.Get(),
                                          brackets.GetCloseLocation());
        }
        break;
      }

      case TokenKind::kLParen: {
        BalancedDelimiterTracker parens(*this, TokenKind::kLParen);
        parens.ConsumeOpen();
        std::vector<Expr*> args;
        bool arg_error = false;
        if (!TokIs(TokenKind::kRParen)) {
          for (;;) {
            ExprResult arg = ParseAssignmentExpression();
            if (arg.IsInvalid()) {
              arg_error = true;
              SkipUntil({TokenKind::kRParen}, kStopBeforeMatch | kStopAtSemi);
              break;
            }
            args.push_back(arg.Get());
            if (!TryConsumeToken(TokenKind::kComma)) break;
          }
        }
        parens.ConsumeClose();
        if (lhs.IsInvalid() || arg_error) {
          lhs = ExprResult::MakeInvalid();
        } else {
          lhs = sema_.ActOnCallExpr(lhs.Get(), std::move(args),
                                    parens.GetCloseLocation());
        }
        break;
      }

      case TokenKind::kPeriod:
      case TokenKind::kArrow: {
        bool is_arrow = TokIs(TokenKind::kArrow);
        SourceLocation op_loc = ConsumeToken();
        if (!TokIs(TokenKind::kIdentifier)) {
          Diag(tok_, diag::err_expected_ident);
          return ExprResult::MakeInvalid();
        }
        const IdentifierInfo* member = tok_.GetIdentifierInfo();
        SourceLocation member_loc = ConsumeToken();
        if (lhs.IsInvalid()) break;
        lhs = sema_.ActOnMemberAccess(lhs.Get(), op_loc, is_arrow, member,
                                      member_loc);
        break;
      }

      case TokenKind::kPlusPlus:
      case TokenKind::kMinusMinus: {
        bool is_inc = TokIs(TokenKind::kPlusPlus);
        SourceLocation loc = ConsumeToken();
        if (lhs.IsInvalid()) break;
        lhs = sema_.ActOnUnaryOp(loc,
                                 is_inc ? UnaryOperatorKind::kPostInc
                                        : UnaryOperatorKind::kPostDec,
                                 lhs.Get());
        break;
      }

      default:
        return lhs;
    }
  }
}

//===----------------------------------------------------------------------===//
// Parenthesized expressions, casts, compound literals.
//===----------------------------------------------------------------------===//

ExprResult Parser::ParseParenExpression(ParenParseOption& parse_kind,
                                        QualType& cast_type,
                                        SourceLocation& rparen_loc) {
  BalancedDelimiterTracker parens(*this, TokenKind::kLParen);
  parens.ConsumeOpen();

  if (IsTypeSpecifierStart(tok_)) {
    cast_type = ParseTypeName();
    parens.ConsumeClose();
    rparen_loc = parens.GetCloseLocation();

    if (TokIs(TokenKind::kLBrace)) {
      // `(T){ ... }` — a compound literal (C11 6.5.2.5).
      ExprResult init = ParseBraceInitializer();
      if (init.IsInvalid() || cast_type.IsNull()) {
        return ExprResult::MakeInvalid();
      }
      parse_kind = ParenParseOption::kCompoundLiteral;
      return sema_.ActOnCompoundLiteral(parens.GetOpenLocation(), cast_type,
                                        rparen_loc, init.Get());
    }

    parse_kind = ParenParseOption::kCastExpr;
    return ExprResult();  // the caller parses the cast operand
  }

  ExprResult inner = ParseExpression();
  parens.ConsumeClose();
  rparen_loc = parens.GetCloseLocation();
  parse_kind = ParenParseOption::kExpression;
  if (inner.IsInvalid()) return ExprResult::MakeInvalid();
  return sema_.ActOnParenExpr(parens.GetOpenLocation(), rparen_loc,
                              inner.Get());
}

//===----------------------------------------------------------------------===//
// sizeof / _Alignof.
//===----------------------------------------------------------------------===//

ExprResult Parser::ParseSizeofAlignofExpression() {
  bool is_sizeof = TokIs(TokenKind::kSizeof);
  SourceLocation op_loc = ConsumeToken();

  // `sizeof ( type-name )`, `sizeof unary-expression`,
  // `_Alignof ( type-name )`.
  if (TokIs(TokenKind::kLParen) && IsTypeSpecifierStart(NextToken())) {
    BalancedDelimiterTracker parens(*this, TokenKind::kLParen);
    parens.ConsumeOpen();
    QualType type = ParseTypeName();
    parens.ConsumeClose();

    // `sizeof (T){...}` — the operand is a compound literal expression.
    if (TokIs(TokenKind::kLBrace)) {
      ExprResult init = ParseBraceInitializer();
      if (init.IsInvalid() || type.IsNull()) return ExprResult::MakeInvalid();
      ExprResult literal = sema_.ActOnCompoundLiteral(
          parens.GetOpenLocation(), type, parens.GetCloseLocation(),
          init.Get());
      literal = ParsePostfixExpressionSuffix(literal);
      if (literal.IsInvalid()) return literal;
      return sema_.ActOnSizeofAlignof(
          op_loc, is_sizeof, QualType(), literal.Get(),
          {op_loc, literal.Get()->GetEndLoc()});
    }

    if (type.IsNull()) return ExprResult::MakeInvalid();
    return sema_.ActOnSizeofAlignof(op_loc, is_sizeof, type, nullptr,
                                    {op_loc, parens.GetCloseLocation()});
  }

  if (!is_sizeof) {
    // _Alignof requires a parenthesized type-name.
    Diag(tok_, diag::err_expected) << "'('";
    return ExprResult::MakeInvalid();
  }

  ExprResult operand = ParseCastExpression(/*is_unary_context=*/true);
  if (operand.IsInvalid()) return operand;
  return sema_.ActOnSizeofAlignof(op_loc, /*is_sizeof=*/true, QualType(),
                                  operand.Get(),
                                  {op_loc, operand.Get()->GetEndLoc()});
}

//===----------------------------------------------------------------------===//
// _Generic (C11 6.5.1.1).
//===----------------------------------------------------------------------===//

ExprResult Parser::ParseGenericSelectionExpression() {
  SourceLocation generic_loc = ConsumeToken();

  BalancedDelimiterTracker parens(*this, TokenKind::kLParen);
  if (parens.ConsumeOpen()) {
    Diag(tok_, diag::err_expected) << "'('";
    return ExprResult::MakeInvalid();
  }

  ExprResult controlling = ParseAssignmentExpression();
  if (controlling.IsInvalid()) {
    SkipUntil({TokenKind::kRParen});
    return ExprResult::MakeInvalid();
  }

  std::vector<Sema::GenericAssoc> assocs;
  while (TryConsumeToken(TokenKind::kComma)) {
    Sema::GenericAssoc assoc;
    assoc.loc = tok_.GetLocation();

    if (TokIs(TokenKind::kDefault)) {
      ConsumeToken();
    } else {
      assoc.type = ParseTypeName();
      if (assoc.type.IsNull()) {
        SkipUntil({TokenKind::kRParen});
        return ExprResult::MakeInvalid();
      }
    }

    if (ExpectAndConsume(TokenKind::kColon, diag::err_expected, "':'")) {
      SkipUntil({TokenKind::kRParen});
      return ExprResult::MakeInvalid();
    }

    ExprResult value = ParseAssignmentExpression();
    if (value.IsInvalid()) {
      SkipUntil({TokenKind::kRParen});
      return ExprResult::MakeInvalid();
    }
    assoc.expr = value.Get();
    assocs.push_back(assoc);
  }

  parens.ConsumeClose();

  ExprResult res = sema_.ActOnGenericSelection(
      generic_loc, controlling.Get(), std::move(assocs),
      parens.GetCloseLocation());
  if (res.IsInvalid()) return res;
  return ParsePostfixExpressionSuffix(res);
}

//===----------------------------------------------------------------------===//
// String literals (with adjacent-literal concatenation).
//===----------------------------------------------------------------------===//

ExprResult Parser::ParseStringLiteralExpression() {
  std::vector<Token> toks;
  while (IsStringLiteralKind(tok_.GetKind())) {
    toks.push_back(tok_);
    ConsumeToken();
  }
  return sema_.ActOnStringLiteral(toks);
}

}  // namespace bcc
