#include <string>

#include "bcc/lex/literal_support.hh"
#include "bcc/lex/numeric_literal.hh"
#include "bcc/lex/token.hh"
#include "bcc/pp/identifier_table.hh"
#include "bcc/sema/sema.hh"

namespace bcc {

namespace {

/// Strips backslash-newline splices from a raw token lexeme.
std::string CleanSpelling(const Token& tok) {
  std::string_view raw = tok.GetLexeme();
  if (!tok.NeedsCleaning()) return std::string(raw);
  std::string out;
  out.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size();) {
    if (raw[i] == '\\' && i + 1 < raw.size()) {
      std::size_t j = i + 1;
      while (j < raw.size() && (raw[j] == ' ' || raw[j] == '\t')) ++j;
      if (j < raw.size() && (raw[j] == '\n' || raw[j] == '\r')) {
        char nl = raw[j];
        i = j + 1;
        if (nl == '\r' && i < raw.size() && raw[i] == '\n') ++i;
        continue;
      }
    }
    out += raw[i++];
  }
  return out;
}

}  // namespace

//===----------------------------------------------------------------------===//
// Conversions (Clang: SemaExpr.cpp / SemaConversions).
//===----------------------------------------------------------------------===//

Expr* Sema::ImpCastExprToType(Expr* e, QualType type, CastKind kind) {
  if (kind == CastKind::kNoOp &&
      e->GetType().GetCanonical() == type.GetCanonical() && !e->IsLValue()) {
    return e;
  }
  return ctx_.New<ImplicitCastExpr>(kind, e, type);
}

ExprResult Sema::DefaultFunctionArrayLvalueConversion(Expr* e) {
  QualType t = e->GetType();
  if (t.IsNull()) return ExprError();

  if (t->IsFunctionType()) {
    return ImpCastExprToType(e, ctx_.GetPointerType(t.WithoutQualifiers()),
                             CastKind::kFunctionToPointerDecay);
  }
  if (t->IsArrayType()) {
    return ImpCastExprToType(e, ctx_.GetPointerType(t->GetArrayElementType()),
                             CastKind::kArrayToPointerDecay);
  }
  if (e->IsLValue()) {
    // The rvalue result has the unqualified version of the lvalue's type
    // (C11 6.3.2.1p2).
    return ImpCastExprToType(e, t.GetCanonical().WithoutQualifiers(),
                             CastKind::kLValueToRValue);
  }
  return e;
}

ExprResult Sema::UsualUnaryConversions(Expr* e) {
  ExprResult conv = DefaultFunctionArrayLvalueConversion(e);
  if (conv.IsInvalid()) return conv;
  e = conv.Get();

  QualType t = e->GetType();
  if (t->IsIntegerType()) {
    QualType promoted = ctx_.GetPromotedIntegerType(t);
    if (promoted.GetCanonical() != t.GetCanonical()) {
      return ImpCastExprToType(e, promoted, CastKind::kIntegralCast);
    }
  }
  return e;
}

ExprResult Sema::DefaultArgumentPromotion(Expr* e) {
  ExprResult conv = UsualUnaryConversions(e);
  if (conv.IsInvalid()) return conv;
  e = conv.Get();
  const auto* bt = e->GetType().GetCanonical().GetTypePtr()->As<BuiltinType>();
  if (bt && bt->GetKind() == BuiltinTypeKind::kFloat) {
    return ImpCastExprToType(e, ctx_.DoubleTy(), CastKind::kFloatingCast);
  }
  return e;
}

namespace {

/// Rank of a floating type for the usual arithmetic conversions.
int FloatRank(QualType t) {
  const auto* bt = t.GetCanonical().GetTypePtr()->As<BuiltinType>();
  if (!bt) return 0;
  switch (bt->GetKind()) {
    case BuiltinTypeKind::kFloat:
      return 1;
    case BuiltinTypeKind::kDouble:
      return 2;
    case BuiltinTypeKind::kLongDouble:
      return 3;
    default:
      return 0;
  }
}

}  // namespace

/// Computes the common type of two *promoted* arithmetic types
/// (C11 6.3.1.8p1) without touching any expression.
static QualType CommonArithmeticType(ASTContext& ctx, QualType lhs,
                                     QualType rhs) {
  lhs = lhs.GetCanonical().WithoutQualifiers();
  rhs = rhs.GetCanonical().WithoutQualifiers();
  if (lhs == rhs) return lhs;

  int lf = FloatRank(lhs);
  int rf = FloatRank(rhs);
  if (lf || rf) return lf >= rf ? (lf ? lhs : rhs) : rhs;

  int lr = ctx.GetIntegerRank(lhs);
  int rr = ctx.GetIntegerRank(rhs);
  bool lu = ctx.IsUnsignedIntegerType(lhs);
  bool ru = ctx.IsUnsignedIntegerType(rhs);

  if (lu == ru) return lr >= rr ? lhs : rhs;

  QualType unsigned_ty = lu ? lhs : rhs;
  QualType signed_ty = lu ? rhs : lhs;
  int unsigned_rank = lu ? lr : rr;
  int signed_rank = lu ? rr : lr;

  if (unsigned_rank >= signed_rank) return unsigned_ty;
  // The signed type has higher rank: it wins if it can represent every value
  // of the unsigned type, i.e. it is strictly wider.
  if (ctx.GetTypeSize(signed_ty) > ctx.GetTypeSize(unsigned_ty)) {
    return signed_ty;
  }
  return ctx.GetCorrespondingUnsignedType(signed_ty);
}

QualType Sema::UsualArithmeticConversions(Expr*& lhs, Expr*& rhs) {
  ExprResult lc = UsualUnaryConversions(lhs);
  ExprResult rc = UsualUnaryConversions(rhs);
  if (lc.IsInvalid() || rc.IsInvalid()) return {};
  lhs = lc.Get();
  rhs = rc.Get();

  QualType lt = lhs->GetType();
  QualType rt = rhs->GetType();
  if (!lt->IsArithmeticType() || !rt->IsArithmeticType()) return {};

  QualType common = CommonArithmeticType(ctx_, lt, rt);

  auto convert = [&](Expr*& e) {
    QualType from = e->GetType();
    if (from.GetCanonical().WithoutQualifiers() == common) return;
    CastKind kind;
    if (from->IsIntegerType()) {
      kind = common->IsFloatingType() ? CastKind::kIntegralToFloating
                                      : CastKind::kIntegralCast;
    } else {
      kind = common->IsFloatingType() ? CastKind::kFloatingCast
                                      : CastKind::kFloatingToIntegral;
    }
    e = ImpCastExprToType(e, common, kind);
  };
  convert(lhs);
  convert(rhs);
  return common;
}

CastKind Sema::GetScalarCastKind(QualType from, QualType to) {
  if (to->IsVoidType()) return CastKind::kToVoid;
  bool from_int = from->IsIntegerType();
  bool from_float = from->IsFloatingType();
  bool from_ptr = from->IsPointerType();
  bool to_bool = to->IsBoolType();

  if (from_int) {
    if (to_bool) return CastKind::kIntegralToBoolean;
    if (to->IsIntegerType()) return CastKind::kIntegralCast;
    if (to->IsFloatingType()) return CastKind::kIntegralToFloating;
    if (to->IsPointerType()) return CastKind::kIntegralToPointer;
  } else if (from_float) {
    if (to_bool) return CastKind::kFloatingToBoolean;
    if (to->IsIntegerType()) return CastKind::kFloatingToIntegral;
    if (to->IsFloatingType()) return CastKind::kFloatingCast;
  } else if (from_ptr) {
    if (to_bool) return CastKind::kPointerToBoolean;
    if (to->IsIntegerType()) return CastKind::kPointerToIntegral;
    if (to->IsPointerType()) return CastKind::kBitCast;
  }
  return CastKind::kNoOp;
}

//===----------------------------------------------------------------------===//
// Assignment compatibility (C11 6.5.16.1).
//===----------------------------------------------------------------------===//

Sema::AssignConvertType Sema::CheckSingleAssignmentConstraints(
    QualType lhs_type, Expr*& rhs, QualType* rhs_type_out) {
  ExprResult conv = DefaultFunctionArrayLvalueConversion(rhs);
  if (conv.IsInvalid()) return AssignConvertType::kIncompatible;
  rhs = conv.Get();
  if (rhs_type_out) *rhs_type_out = rhs->GetType();

  QualType lt = lhs_type.GetCanonical().WithoutQualifiers();
  QualType rt = rhs->GetType().GetCanonical().WithoutQualifiers();

  if (lt == rt) {
    rhs = ImpCastExprToType(rhs, lt, CastKind::kNoOp);
    return AssignConvertType::kCompatible;
  }

  // Arithmetic <- arithmetic.
  if (lt->IsArithmeticType() && rt->IsArithmeticType()) {
    rhs = ImpCastExprToType(rhs, lt, GetScalarCastKind(rt, lt));
    return AssignConvertType::kCompatible;
  }

  // _Bool <- pointer.
  if (lt->IsBoolType() && rt->IsPointerType()) {
    rhs = ImpCastExprToType(rhs, lt, CastKind::kPointerToBoolean);
    return AssignConvertType::kCompatible;
  }

  if (lt->IsPointerType()) {
    if (rhs->IsNullPointerConstant()) {
      rhs = ImpCastExprToType(rhs, lt, CastKind::kNullToPointer);
      return AssignConvertType::kCompatible;
    }
    if (rt->IsPointerType()) {
      QualType lp = lt->GetPointeeType().GetCanonical();
      QualType rp = rt->GetPointeeType().GetCanonical();
      bool pointees_compatible =
          ctx_.IsCompatible(lp.WithoutQualifiers(), rp.WithoutQualifiers()) ||
          lp.GetTypePtr()->IsVoidType() || rp.GetTypePtr()->IsVoidType();
      // void* does not mix with function pointers.
      if ((lp.GetTypePtr()->IsVoidType() &&
           rp.GetTypePtr()->IsFunctionType()) ||
          (rp.GetTypePtr()->IsVoidType() &&
           lp.GetTypePtr()->IsFunctionType())) {
        pointees_compatible = false;
      }
      if (pointees_compatible) {
        if (!lp.GetQualifiers().Contains(rp.GetQualifiers())) {
          rhs = ImpCastExprToType(rhs, lt, CastKind::kBitCast);
          return AssignConvertType::kDiscardsQualifiers;
        }
        rhs = ImpCastExprToType(rhs, lt, CastKind::kBitCast);
        return AssignConvertType::kCompatible;
      }
      rhs = ImpCastExprToType(rhs, lt, CastKind::kBitCast);
      return AssignConvertType::kIncompatiblePointer;
    }
    if (rt->IsIntegerType()) {
      rhs = ImpCastExprToType(rhs, lt, CastKind::kIntegralToPointer);
      return AssignConvertType::kPointerInt;
    }
    return AssignConvertType::kIncompatible;
  }

  if (lt->IsIntegerType() && rt->IsPointerType()) {
    rhs = ImpCastExprToType(rhs, lt, CastKind::kPointerToIntegral);
    return AssignConvertType::kPointerInt;
  }

  // Struct/union <- compatible struct/union.
  if (lt->IsRecordType() && ctx_.IsCompatible(lt, rt)) {
    rhs = ImpCastExprToType(rhs, lt, CastKind::kNoOp);
    return AssignConvertType::kCompatible;
  }

  return AssignConvertType::kIncompatible;
}

void Sema::DiagnoseAssignmentResult(AssignConvertType result,
                                    SourceLocation loc, QualType lhs_type,
                                    QualType rhs_type,
                                    std::string_view action) {
  std::string_view connector = action == "initializing" ? "with" : "from";
  switch (result) {
    case AssignConvertType::kCompatible:
      return;
    case AssignConvertType::kPointerInt:
      Diag(loc, lhs_type->IsPointerType()
                    ? diag::warn_typecheck_convert_int_pointer
                    : diag::warn_typecheck_convert_pointer_int)
          << lhs_type.GetAsString() << rhs_type.GetAsString() << action
          << connector;
      return;
    case AssignConvertType::kIncompatiblePointer:
      Diag(loc, diag::warn_typecheck_convert_incompatible_pointer)
          << lhs_type.GetAsString() << rhs_type.GetAsString() << action
          << connector;
      return;
    case AssignConvertType::kDiscardsQualifiers:
      Diag(loc, diag::warn_typecheck_convert_discards_qualifiers)
          << lhs_type.GetAsString() << rhs_type.GetAsString() << action
          << connector;
      return;
    case AssignConvertType::kIncompatible:
      Diag(loc, diag::err_typecheck_convert_incompatible)
          << lhs_type.GetAsString() << rhs_type.GetAsString() << action
          << connector;
      return;
  }
}

ExprResult Sema::CheckBooleanCondition(Expr* e, SourceLocation stmt_loc) {
  ExprResult conv = DefaultFunctionArrayLvalueConversion(e);
  if (conv.IsInvalid()) return conv;
  e = conv.Get();
  QualType t = e->GetType();
  if (!t->IsScalarType()) {
    Diag(stmt_loc, diag::err_typecheck_statement_requires_scalar)
        << t.GetAsString();
    return ExprError();
  }
  return e;
}

//===----------------------------------------------------------------------===//
// Primary expressions.
//===----------------------------------------------------------------------===//

ExprResult Sema::ActOnIdExpression(Scope* s, const IdentifierInfo* name,
                                   SourceLocation loc, bool is_callee) {
  (void)s;
  NamedDecl* d = LookupOrdinaryName(name);
  if (!d) {
    Diag(loc, is_callee ? diag::err_implicit_function_decl
                        : diag::err_undeclared_var_use)
        << name->GetName();
    return ExprError();
  }

  if (auto* td = d->As<TypedefDecl>()) {
    (void)td;
    Diag(loc, diag::err_unexpected_typedef_ident) << name->GetName();
    return ExprError();
  }

  auto* vd = d->As<ValueDecl>();
  if (!vd) {
    Diag(loc, diag::err_undeclared_var_use) << name->GetName();
    return ExprError();
  }

  ValueKind vk = vd->GetKind() == DeclKind::kEnumConstant ? ValueKind::kRValue
                                                          : ValueKind::kLValue;
  return ctx_.New<DeclRefExpr>(vd, vd->GetType(), vk, loc);
}

ExprResult Sema::ActOnNumericConstant(const Token& tok) {
  std::string spelling = CleanSpelling(tok);
  NumericLiteralParser literal(spelling);
  if (literal.HadError()) {
    switch (literal.GetError()) {
      case NumericLiteralParser::Error::kInvalidSuffix:
        Diag(tok.GetLocation(), diag::err_invalid_suffix_constant)
            << spelling
            << (literal.IsIntegerLiteral() ? "integer" : "floating");
        break;
      case NumericLiteralParser::Error::kMissingExponentDigits:
        Diag(tok.GetLocation(), diag::err_exponent_has_no_digits);
        break;
      default:
        Diag(tok.GetLocation(), diag::err_invalid_digit)
            << spelling << "numeric";
        break;
    }
    return ExprError();
  }

  if (literal.IsFloatingLiteral()) {
    auto value = literal.GetFloatValue();
    if (!value) return ExprError();

    QualType type = ctx_.DoubleTy();
    if (value.Value().GetFormat() == fmt::kFloat) type = ctx_.FloatTy();
    if (literal.IsLong()) type = ctx_.LongDoubleTy();
    return ctx_.New<FloatingLiteral>(value.Value().ToDouble(), type,
                                     tok.GetLocation());
  }

  APSInt parsed_value;
  if (literal.GetIntegerValue(parsed_value, 64)) {
    Diag(tok.GetLocation(), diag::err_integer_literal_too_large);
    return ExprError();
  }
  uint64_t value = parsed_value.GetZExtValue();

  // C11 6.4.4.1p5: the type is the first in the applicable list that can
  // represent the value.
  bool allow_unsigned = literal.IsUnsigned() || literal.GetRadix() != 10;
  QualType type;
  auto fits_signed = [&](QualType t) {
    uint64_t bits = ctx_.GetIntWidth(t);
    return value <= (uint64_t{1} << (bits - 1)) - 1;
  };
  auto fits_unsigned = [&](QualType t) {
    uint64_t bits = ctx_.GetIntWidth(t);
    return bits >= 64 || value <= (uint64_t{1} << bits) - 1;
  };

  if (!literal.IsLong() && !literal.IsLongLong()) {
    if (!literal.IsUnsigned() && fits_signed(ctx_.IntTy())) {
      type = ctx_.IntTy();
    } else if (allow_unsigned && fits_unsigned(ctx_.UIntTy())) {
      type = ctx_.UIntTy();
    }
  }
  if (type.IsNull() && !literal.IsLongLong()) {
    if (!literal.IsUnsigned() && fits_signed(ctx_.LongTy())) {
      type = ctx_.LongTy();
    } else if (allow_unsigned && fits_unsigned(ctx_.ULongTy())) {
      type = ctx_.ULongTy();
    }
  }
  if (type.IsNull()) {
    if (!literal.IsUnsigned() && fits_signed(ctx_.LongLongTy())) {
      type = ctx_.LongLongTy();
    } else if (allow_unsigned) {
      type = ctx_.ULongLongTy();
      if (!literal.IsUnsigned()) {
        Diag(tok.GetLocation(), diag::warn_integer_literal_too_large);
      }
    } else {
      // Decimal with no 'u' suffix that only fits unsigned.
      Diag(tok.GetLocation(), diag::warn_integer_literal_too_large);
      type = ctx_.ULongLongTy();
    }
  }

  return ctx_.New<IntegerLiteral>(value, type, tok.GetLocation());
}

ExprResult Sema::ActOnCharacterConstant(const Token& tok) {
  std::string spelling = CleanSpelling(tok);
  CharLiteralParser literal(spelling, tok.GetLocation(), tok.GetKind(), diags_);
  if (literal.HadError()) return ExprError();

  QualType type;
  switch (tok.GetKind()) {
    case TokenKind::kUtf16CharConstant:
      type = ctx_.Char16Ty();
      break;
    case TokenKind::kUtf32CharConstant:
      type = ctx_.Char32Ty();
      break;
    // Plain and wide character constants have type int in C
    // (C11 6.4.4.4p10; wchar_t is int on x86-64).
    default:
      type = ctx_.IntTy();
      break;
  }
  return ctx_.New<CharacterLiteral>(literal.GetValue(), type,
                                    tok.GetLocation());
}

ExprResult Sema::ActOnStringLiteral(const std::vector<Token>& toks) {
  std::vector<StringLiteralParser::Piece> pieces;
  pieces.reserve(toks.size());
  for (const Token& t : toks) {
    pieces.push_back({CleanSpelling(t), t.GetLocation(), t.GetKind()});
  }
  StringLiteralParser literal(pieces, diags_);
  if (literal.HadError()) return ExprError();

  QualType elem;
  switch (literal.GetKind()) {
    case TokenKind::kUtf16StringLiteral:
      elem = ctx_.Char16Ty();
      break;
    case TokenKind::kUtf32StringLiteral:
      elem = ctx_.Char32Ty();
      break;
    case TokenKind::kWideStringLiteral:
      elem = ctx_.WCharTy();
      break;
    default:
      elem = ctx_.CharTy();
      break;
  }

  QualType type =
      ctx_.GetStringLiteralArrayType(elem, literal.GetNumElements());
  return ctx_.New<StringLiteral>(literal.GetBytes(), literal.GetCharByteWidth(),
                                 type, toks.front().GetLocation());
}

ExprResult Sema::ActOnParenExpr(SourceLocation lparen, SourceLocation rparen,
                                Expr* sub) {
  return ctx_.New<ParenExpr>(sub, lparen, rparen);
}

//===----------------------------------------------------------------------===//
// Unary operators.
//===----------------------------------------------------------------------===//

bool Sema::CheckModifiableLValue(Expr* e, SourceLocation loc) {
  if (e->IsModifiableLValue()) return true;

  if (e->IsLValue()) {
    // Const-qualified: name the variable when we can.
    const Expr* inner = e->IgnoreParens();
    if (const auto* ref = inner->As<DeclRefExpr>()) {
      if (ref->GetDecl()->As<VarDecl>() &&
          ref->GetType().GetCanonical().HasConst()) {
        Diag(loc, diag::err_typecheck_assign_const)
            << ref->GetDecl()->GetName() << ref->GetType().GetAsString();
        return false;
      }
    }
  }
  Diag(loc, diag::err_typecheck_expression_not_modifiable_lvalue);
  return false;
}

QualType Sema::CheckIncrementDecrementOperand(Expr*& operand,
                                              SourceLocation loc,
                                              bool is_increment) {
  QualType t = operand->GetType();
  if (t.IsNull()) return {};

  bool ok = t->IsArithmeticType();
  if (const auto* pt = t.GetCanonical().GetTypePtr()->As<PointerType>()) {
    QualType pointee = pt->GetPointee();
    if (pointee.GetTypePtr()->IsFunctionType()) {
      Diag(loc, diag::err_typecheck_pointer_arith_function_type)
          << pointee.GetAsString();
      return {};
    }
    if (!pointee.GetTypePtr()->IsCompleteType()) {
      Diag(loc, diag::err_typecheck_arithmetic_incomplete_type)
          << pointee.GetAsString();
      return {};
    }
    ok = true;
  }
  if (!ok) {
    Diag(loc, diag::err_typecheck_illegal_increment_decrement)
        << t.GetAsString() << (is_increment ? "increment" : "decrement");
    return {};
  }
  if (!CheckModifiableLValue(operand, loc)) return {};
  return t.GetCanonical().WithoutQualifiers();
}

ExprResult Sema::CheckAddressOfOperand(Expr* operand, SourceLocation loc) {
  QualType t = operand->GetType();
  if (t.IsNull()) return ExprError();

  // &* and &[] fold: &*p is p, &a[i] is a+i — but keeping the explicit form
  // in the AST is also valid; we only need the type. Function designators
  // and lvalues are addressable.
  if (!operand->IsLValue() && !t->IsFunctionType()) {
    Diag(loc, diag::err_typecheck_invalid_lvalue_addrof) << t.GetAsString();
    return ExprError();
  }

  const Expr* inner = operand->IgnoreParens();
  if (const auto* me = inner->As<MemberExpr>()) {
    if (me->GetMember()->IsBitField()) {
      Diag(loc, diag::err_typecheck_addrof_bitfield);
      return ExprError();
    }
  }
  if (const auto* ref = inner->As<DeclRefExpr>()) {
    if (const auto* vd = ref->GetDecl()->As<VarDecl>()) {
      if (vd->GetStorageClass() == StorageClass::kRegister) {
        Diag(loc, diag::err_typecheck_address_of_register);
        return ExprError();
      }
    }
  }

  return ctx_.New<UnaryOperator>(UnaryOperatorKind::kAddrOf, operand,
                                 ctx_.GetPointerType(t), ValueKind::kRValue,
                                 loc);
}

ExprResult Sema::CheckIndirectionOperand(Expr* operand, SourceLocation loc) {
  ExprResult conv = DefaultFunctionArrayLvalueConversion(operand);
  if (conv.IsInvalid()) return conv;
  operand = conv.Get();

  QualType t = operand->GetType();
  const auto* pt = t.GetCanonical().GetTypePtr()->As<PointerType>();
  if (!pt) {
    Diag(loc, diag::err_typecheck_indirection_requires_pointer)
        << t.GetAsString();
    return ExprError();
  }
  // Dereferencing a function pointer yields a function designator (lvalue in
  // our representation).
  return ctx_.New<UnaryOperator>(UnaryOperatorKind::kDeref, operand,
                                 pt->GetPointee(), ValueKind::kLValue, loc);
}

ExprResult Sema::ActOnUnaryOp(SourceLocation op_loc, UnaryOperatorKind op,
                              Expr* operand) {
  if (!operand) return ExprError();

  switch (op) {
    case UnaryOperatorKind::kPreInc:
    case UnaryOperatorKind::kPreDec:
    case UnaryOperatorKind::kPostInc:
    case UnaryOperatorKind::kPostDec: {
      bool is_inc =
          op == UnaryOperatorKind::kPreInc || op == UnaryOperatorKind::kPostInc;
      QualType t = CheckIncrementDecrementOperand(operand, op_loc, is_inc);
      if (t.IsNull()) return ExprError();
      return ctx_.New<UnaryOperator>(op, operand, t, ValueKind::kRValue,
                                     op_loc);
    }
    case UnaryOperatorKind::kAddrOf:
      return CheckAddressOfOperand(operand, op_loc);
    case UnaryOperatorKind::kDeref:
      return CheckIndirectionOperand(operand, op_loc);
    case UnaryOperatorKind::kPlus:
    case UnaryOperatorKind::kMinus: {
      ExprResult conv = UsualUnaryConversions(operand);
      if (conv.IsInvalid()) return conv;
      operand = conv.Get();
      if (!operand->GetType()->IsArithmeticType()) {
        Diag(op_loc, diag::err_typecheck_unary_expr)
            << operand->GetType().GetAsString();
        return ExprError();
      }
      return ctx_.New<UnaryOperator>(op, operand, operand->GetType(),
                                     ValueKind::kRValue, op_loc);
    }
    case UnaryOperatorKind::kNot: {
      ExprResult conv = UsualUnaryConversions(operand);
      if (conv.IsInvalid()) return conv;
      operand = conv.Get();
      if (!operand->GetType()->IsIntegerType()) {
        Diag(op_loc, diag::err_typecheck_unary_expr)
            << operand->GetType().GetAsString();
        return ExprError();
      }
      return ctx_.New<UnaryOperator>(op, operand, operand->GetType(),
                                     ValueKind::kRValue, op_loc);
    }
    case UnaryOperatorKind::kLNot: {
      ExprResult conv = CheckBooleanCondition(operand, op_loc);
      if (conv.IsInvalid()) return conv;
      // The result of ! has type int (C11 6.5.3.3p5).
      return ctx_.New<UnaryOperator>(op, conv.Get(), ctx_.IntTy(),
                                     ValueKind::kRValue, op_loc);
    }
  }
  return ExprError();
}

//===----------------------------------------------------------------------===//
// Binary operators (Clang: SemaExpr.cpp CreateBuiltinBinOp and Check*).
//===----------------------------------------------------------------------===//

void Sema::DiagnoseBadBinaryOperands(SourceLocation loc, const Expr* lhs,
                                     const Expr* rhs) {
  Diag(loc, diag::err_typecheck_invalid_operands)
      << lhs->GetType().GetAsString() << rhs->GetType().GetAsString();
}

QualType Sema::CheckMultiplyDivideOperands(Expr*& lhs, Expr*& rhs,
                                           SourceLocation loc, bool is_div) {
  (void)is_div;
  QualType common = UsualArithmeticConversions(lhs, rhs);
  if (common.IsNull()) {
    DiagnoseBadBinaryOperands(loc, lhs, rhs);
    return {};
  }
  return common;
}

QualType Sema::CheckRemainderOperands(Expr*& lhs, Expr*& rhs,
                                      SourceLocation loc) {
  if (!lhs->GetType()->IsIntegerType() || !rhs->GetType()->IsIntegerType()) {
    // Give the operands their converted form for the diagnostic.
    QualType common = UsualArithmeticConversions(lhs, rhs);
    (void)common;
    DiagnoseBadBinaryOperands(loc, lhs, rhs);
    return {};
  }
  return UsualArithmeticConversions(lhs, rhs);
}

/// Checks pointer arithmetic validity: pointee must be a complete object
/// type. Returns false (diagnosing) otherwise.
static bool CheckPointerArithmeticOperand(Sema& sema, QualType pointer_type,
                                          SourceLocation loc) {
  QualType pointee = pointer_type.GetCanonical().GetTypePtr()->GetPointeeType();
  if (pointee.GetTypePtr()->IsFunctionType()) {
    sema.Diag(loc, diag::err_typecheck_pointer_arith_function_type)
        << pointee.GetAsString();
    return false;
  }
  if (!pointee.GetTypePtr()->IsCompleteType()) {
    sema.Diag(loc, diag::err_typecheck_arithmetic_incomplete_type)
        << pointee.GetAsString();
    return false;
  }
  return true;
}

QualType Sema::CheckAdditionOperands(Expr*& lhs, Expr*& rhs,
                                     SourceLocation loc) {
  ExprResult lc = UsualUnaryConversions(lhs);
  ExprResult rc = UsualUnaryConversions(rhs);
  if (lc.IsInvalid() || rc.IsInvalid()) return {};
  lhs = lc.Get();
  rhs = rc.Get();

  QualType lt = lhs->GetType();
  QualType rt = rhs->GetType();

  if (lt->IsArithmeticType() && rt->IsArithmeticType()) {
    return UsualArithmeticConversions(lhs, rhs);
  }

  // pointer + integer (either order).
  Expr* pointer =
      lt->IsPointerType() ? lhs : (rt->IsPointerType() ? rhs : nullptr);
  Expr* index = pointer == lhs ? rhs : lhs;
  if (pointer && index->GetType()->IsIntegerType()) {
    if (!CheckPointerArithmeticOperand(*this, pointer->GetType(), loc)) {
      return {};
    }
    return pointer->GetType();
  }

  DiagnoseBadBinaryOperands(loc, lhs, rhs);
  return {};
}

QualType Sema::CheckSubtractionOperands(Expr*& lhs, Expr*& rhs,
                                        SourceLocation loc) {
  ExprResult lc = UsualUnaryConversions(lhs);
  ExprResult rc = UsualUnaryConversions(rhs);
  if (lc.IsInvalid() || rc.IsInvalid()) return {};
  lhs = lc.Get();
  rhs = rc.Get();

  QualType lt = lhs->GetType();
  QualType rt = rhs->GetType();

  if (lt->IsArithmeticType() && rt->IsArithmeticType()) {
    return UsualArithmeticConversions(lhs, rhs);
  }

  if (lt->IsPointerType()) {
    if (rt->IsIntegerType()) {
      if (!CheckPointerArithmeticOperand(*this, lt, loc)) return {};
      return lt;
    }
    if (rt->IsPointerType()) {
      QualType lp = lt->GetPointeeType().GetCanonical().WithoutQualifiers();
      QualType rp = rt->GetPointeeType().GetCanonical().WithoutQualifiers();
      if (!ctx_.IsCompatible(lp, rp)) {
        Diag(loc, diag::err_typecheck_sub_ptr_compatible)
            << lt.GetAsString() << rt.GetAsString();
        return {};
      }
      if (!CheckPointerArithmeticOperand(*this, lt, loc)) return {};
      return ctx_.PtrdiffTy();
    }
  }

  DiagnoseBadBinaryOperands(loc, lhs, rhs);
  return {};
}

QualType Sema::CheckShiftOperands(Expr*& lhs, Expr*& rhs, SourceLocation loc) {
  // Shifts promote each operand independently; the result has the promoted
  // LHS type (C11 6.5.7p3).
  ExprResult lc = UsualUnaryConversions(lhs);
  ExprResult rc = UsualUnaryConversions(rhs);
  if (lc.IsInvalid() || rc.IsInvalid()) return {};
  lhs = lc.Get();
  rhs = rc.Get();
  if (!lhs->GetType()->IsIntegerType() || !rhs->GetType()->IsIntegerType()) {
    DiagnoseBadBinaryOperands(loc, lhs, rhs);
    return {};
  }
  return lhs->GetType();
}

QualType Sema::CheckCompareOperands(Expr*& lhs, Expr*& rhs, SourceLocation loc,
                                    BinaryOperatorKind op) {
  bool is_equality =
      op == BinaryOperatorKind::kEQ || op == BinaryOperatorKind::kNE;

  ExprResult lc = UsualUnaryConversions(lhs);
  ExprResult rc = UsualUnaryConversions(rhs);
  if (lc.IsInvalid() || rc.IsInvalid()) return {};
  lhs = lc.Get();
  rhs = rc.Get();

  QualType lt = lhs->GetType();
  QualType rt = rhs->GetType();

  if (lt->IsArithmeticType() && rt->IsArithmeticType()) {
    if (UsualArithmeticConversions(lhs, rhs).IsNull()) return {};
    return ctx_.IntTy();
  }

  if (lt->IsPointerType() && rt->IsPointerType()) {
    QualType lp = lt->GetPointeeType().GetCanonical().WithoutQualifiers();
    QualType rp = rt->GetPointeeType().GetCanonical().WithoutQualifiers();
    bool void_mix =
        lp.GetTypePtr()->IsVoidType() || rp.GetTypePtr()->IsVoidType();
    if (!ctx_.IsCompatible(lp, rp) && !(is_equality && void_mix)) {
      Diag(loc, diag::err_typecheck_comparison_of_distinct_pointers)
          << lt.GetAsString() << rt.GetAsString();
    }
    if (rt.GetCanonical() != lt.GetCanonical()) {
      rhs = ImpCastExprToType(rhs, lt, CastKind::kBitCast);
    }
    return ctx_.IntTy();
  }

  // Pointer vs null pointer constant.
  if (lt->IsPointerType() && rhs->IsNullPointerConstant()) {
    rhs = ImpCastExprToType(rhs, lt, CastKind::kNullToPointer);
    return ctx_.IntTy();
  }
  if (rt->IsPointerType() && lhs->IsNullPointerConstant()) {
    lhs = ImpCastExprToType(lhs, rt, CastKind::kNullToPointer);
    return ctx_.IntTy();
  }

  // Pointer vs integer: allowed with a warning.
  if (lt->IsPointerType() && rt->IsIntegerType()) {
    Diag(loc, diag::warn_typecheck_comparison_of_pointer_integer)
        << lt.GetAsString() << rt.GetAsString();
    rhs = ImpCastExprToType(rhs, lt, CastKind::kIntegralToPointer);
    return ctx_.IntTy();
  }
  if (lt->IsIntegerType() && rt->IsPointerType()) {
    Diag(loc, diag::warn_typecheck_comparison_of_pointer_integer)
        << lt.GetAsString() << rt.GetAsString();
    lhs = ImpCastExprToType(lhs, rt, CastKind::kIntegralToPointer);
    return ctx_.IntTy();
  }

  DiagnoseBadBinaryOperands(loc, lhs, rhs);
  return {};
}

QualType Sema::CheckBitwiseOperands(Expr*& lhs, Expr*& rhs,
                                    SourceLocation loc) {
  if (!lhs->GetType().IsNull() && !rhs->GetType().IsNull()) {
    QualType common = UsualArithmeticConversions(lhs, rhs);
    if (!common.IsNull() && common->IsIntegerType()) return common;
  }
  DiagnoseBadBinaryOperands(loc, lhs, rhs);
  return {};
}

QualType Sema::CheckLogicalOperands(Expr*& lhs, Expr*& rhs,
                                    SourceLocation loc) {
  ExprResult lc = CheckBooleanCondition(lhs, loc);
  ExprResult rc = CheckBooleanCondition(rhs, loc);
  if (lc.IsInvalid() || rc.IsInvalid()) return {};
  lhs = lc.Get();
  rhs = rc.Get();
  return ctx_.IntTy();
}

QualType Sema::CheckAssignmentOperands(Expr* lhs, Expr*& rhs,
                                       SourceLocation loc) {
  if (!CheckModifiableLValue(lhs, loc)) return {};

  QualType lhs_type = lhs->GetType();
  QualType rhs_type = rhs->GetType();
  AssignConvertType result =
      CheckSingleAssignmentConstraints(lhs_type, rhs, &rhs_type);
  DiagnoseAssignmentResult(result, loc, lhs_type, rhs_type, "assigning to");
  if (result == AssignConvertType::kIncompatible) return {};

  // The result type of an assignment is the (unqualified) LHS type.
  return lhs_type.GetCanonical().WithoutQualifiers();
}

ExprResult Sema::ActOnBinOp(SourceLocation op_loc, BinaryOperatorKind op,
                            Expr* lhs, Expr* rhs) {
  if (!lhs || !rhs) return ExprError();

  QualType result_type;
  ValueKind vk = ValueKind::kRValue;

  switch (op) {
    case BinaryOperatorKind::kMul:
    case BinaryOperatorKind::kDiv:
      result_type = CheckMultiplyDivideOperands(lhs, rhs, op_loc,
                                                op == BinaryOperatorKind::kDiv);
      break;
    case BinaryOperatorKind::kRem:
      result_type = CheckRemainderOperands(lhs, rhs, op_loc);
      break;
    case BinaryOperatorKind::kAdd:
      result_type = CheckAdditionOperands(lhs, rhs, op_loc);
      break;
    case BinaryOperatorKind::kSub:
      result_type = CheckSubtractionOperands(lhs, rhs, op_loc);
      break;
    case BinaryOperatorKind::kShl:
    case BinaryOperatorKind::kShr:
      result_type = CheckShiftOperands(lhs, rhs, op_loc);
      break;
    case BinaryOperatorKind::kLT:
    case BinaryOperatorKind::kGT:
    case BinaryOperatorKind::kLE:
    case BinaryOperatorKind::kGE:
    case BinaryOperatorKind::kEQ:
    case BinaryOperatorKind::kNE:
      result_type = CheckCompareOperands(lhs, rhs, op_loc, op);
      break;
    case BinaryOperatorKind::kAnd:
    case BinaryOperatorKind::kXor:
    case BinaryOperatorKind::kOr:
      result_type = CheckBitwiseOperands(lhs, rhs, op_loc);
      break;
    case BinaryOperatorKind::kLAnd:
    case BinaryOperatorKind::kLOr:
      result_type = CheckLogicalOperands(lhs, rhs, op_loc);
      break;
    case BinaryOperatorKind::kAssign:
      result_type = CheckAssignmentOperands(lhs, rhs, op_loc);
      break;
    case BinaryOperatorKind::kComma: {
      // The left operand is evaluated for side effects; the right operand
      // (converted) gives the type and value (C11 6.5.17).
      ExprResult rc = DefaultFunctionArrayLvalueConversion(rhs);
      if (rc.IsInvalid()) return ExprError();
      rhs = rc.Get();
      result_type = rhs->GetType();
      break;
    }
    default: {
      // Compound assignment: op= is checked like `lhs op rhs`, but the LHS
      // remains an lvalue and the result has the LHS type (C11 6.5.16.2).
      if (!CheckModifiableLValue(lhs, op_loc)) return ExprError();

      QualType lhs_type = lhs->GetType();
      QualType lhs_value_type =
          ctx_.GetDecayedType(lhs_type).GetCanonical().WithoutQualifiers();
      QualType rhs_type = rhs->GetType();

      QualType computation_type;
      bool needs_int_rhs = op == BinaryOperatorKind::kRemAssign ||
                           op == BinaryOperatorKind::kShlAssign ||
                           op == BinaryOperatorKind::kShrAssign ||
                           op == BinaryOperatorKind::kAndAssign ||
                           op == BinaryOperatorKind::kXorAssign ||
                           op == BinaryOperatorKind::kOrAssign;

      bool ptr_arith = (op == BinaryOperatorKind::kAddAssign ||
                        op == BinaryOperatorKind::kSubAssign) &&
                       lhs_value_type.GetTypePtr()->IsPointerType();

      ExprResult rc = UsualUnaryConversions(rhs);
      if (rc.IsInvalid()) return ExprError();
      rhs = rc.Get();
      rhs_type = rhs->GetType();

      if (ptr_arith) {
        if (!rhs_type->IsIntegerType() ||
            !CheckPointerArithmeticOperand(*this, lhs_value_type, op_loc)) {
          DiagnoseBadBinaryOperands(op_loc, lhs, rhs);
          return ExprError();
        }
        computation_type = lhs_value_type;
      } else if (lhs_value_type.GetTypePtr()->IsArithmeticType() &&
                 rhs_type->IsArithmeticType() &&
                 (!needs_int_rhs ||
                  (lhs_value_type.GetTypePtr()->IsIntegerType() &&
                   rhs_type->IsIntegerType()))) {
        if (op == BinaryOperatorKind::kShlAssign ||
            op == BinaryOperatorKind::kShrAssign) {
          computation_type = ctx_.GetPromotedIntegerType(lhs_value_type);
        } else {
          computation_type =
              CommonArithmeticType(ctx_, lhs_value_type, rhs_type);
          // Convert the RHS to the computation type.
          if (rhs_type.GetCanonical().WithoutQualifiers() != computation_type) {
            rhs = ImpCastExprToType(
                rhs, computation_type,
                GetScalarCastKind(rhs_type, computation_type));
          }
        }
      } else {
        DiagnoseBadBinaryOperands(op_loc, lhs, rhs);
        return ExprError();
      }

      return ctx_.New<CompoundAssignOperator>(op, lhs, rhs, lhs_value_type,
                                              computation_type, op_loc);
    }
  }

  if (result_type.IsNull()) return ExprError();
  return ctx_.New<BinaryOperator>(op, lhs, rhs, result_type, vk, op_loc);
}

//===----------------------------------------------------------------------===//
// Conditional operator (C11 6.5.15).
//===----------------------------------------------------------------------===//

ExprResult Sema::ActOnConditionalOp(SourceLocation question_loc,
                                    SourceLocation colon_loc, Expr* cond,
                                    Expr* lhs, Expr* rhs) {
  (void)colon_loc;
  if (!cond || !lhs || !rhs) return ExprError();

  ExprResult cond_conv = CheckBooleanCondition(cond, question_loc);
  if (cond_conv.IsInvalid()) return ExprError();
  cond = cond_conv.Get();

  ExprResult lc = UsualUnaryConversions(lhs);
  ExprResult rc = UsualUnaryConversions(rhs);
  if (lc.IsInvalid() || rc.IsInvalid()) return ExprError();
  lhs = lc.Get();
  rhs = rc.Get();

  QualType lt = lhs->GetType();
  QualType rt = rhs->GetType();
  QualType result;

  if (lt->IsArithmeticType() && rt->IsArithmeticType()) {
    result = UsualArithmeticConversions(lhs, rhs);
  } else if (lt->IsVoidType() && rt->IsVoidType()) {
    result = ctx_.VoidTy();
  } else if (lt->IsRecordType() &&
             ctx_.IsCompatible(lt.GetCanonical().WithoutQualifiers(),
                               rt.GetCanonical().WithoutQualifiers())) {
    result = lt;
  } else if (lt->IsPointerType() && rhs->IsNullPointerConstant()) {
    rhs = ImpCastExprToType(rhs, lt, CastKind::kNullToPointer);
    result = lt;
  } else if (rt->IsPointerType() && lhs->IsNullPointerConstant()) {
    lhs = ImpCastExprToType(lhs, rt, CastKind::kNullToPointer);
    result = rt;
  } else if (lt->IsPointerType() && rt->IsPointerType()) {
    QualType lp = lt->GetPointeeType();
    QualType rp = rt->GetPointeeType();
    Qualifiers merged = lp.GetCanonical().GetQualifiers();
    merged.Add(rp.GetCanonical().GetQualifiers().GetMask());

    if (ctx_.IsCompatible(lp.GetCanonical().WithoutQualifiers(),
                          rp.GetCanonical().WithoutQualifiers())) {
      QualType composite =
          ctx_.GetCompositeType(lp.GetCanonical().WithoutQualifiers(),
                                rp.GetCanonical().WithoutQualifiers());
      result = ctx_.GetPointerType(composite.WithQualifiers(merged));
    } else if (lp.GetTypePtr()->IsVoidType() || rp.GetTypePtr()->IsVoidType()) {
      result = ctx_.GetPointerType(ctx_.VoidTy().WithQualifiers(merged));
    } else {
      Diag(question_loc, diag::err_typecheck_cond_incompatible_operands)
          << lt.GetAsString() << rt.GetAsString();
      return ExprError();
    }
    if (lt.GetCanonical() != result.GetCanonical()) {
      lhs = ImpCastExprToType(lhs, result, CastKind::kBitCast);
    }
    if (rt.GetCanonical() != result.GetCanonical()) {
      rhs = ImpCastExprToType(rhs, result, CastKind::kBitCast);
    }
  } else {
    Diag(question_loc, diag::err_typecheck_cond_incompatible_operands)
        << lt.GetAsString() << rt.GetAsString();
    return ExprError();
  }

  if (result.IsNull()) return ExprError();
  return ctx_.New<ConditionalOperator>(cond, lhs, rhs, result);
}

//===----------------------------------------------------------------------===//
// Postfix expressions.
//===----------------------------------------------------------------------===//

ExprResult Sema::ActOnArraySubscript(Expr* base, SourceLocation lsquare,
                                     Expr* idx, SourceLocation rsquare) {
  (void)lsquare;
  if (!base || !idx) return ExprError();

  ExprResult bc = DefaultFunctionArrayLvalueConversion(base);
  ExprResult ic = DefaultFunctionArrayLvalueConversion(idx);
  if (bc.IsInvalid() || ic.IsInvalid()) return ExprError();
  base = bc.Get();
  idx = ic.Get();

  // E1[E2] where either operand may be the pointer (C11 6.5.2.1).
  Expr* pointer = base;
  Expr* integer = idx;
  if (!pointer->GetType()->IsPointerType() &&
      integer->GetType()->IsPointerType()) {
    std::swap(pointer, integer);
  }

  if (!pointer->GetType()->IsPointerType()) {
    Diag(base->GetBeginLoc(), diag::err_typecheck_subscript_value);
    return ExprError();
  }
  if (!integer->GetType()->IsIntegerType()) {
    Diag(idx->GetBeginLoc(), diag::err_typecheck_subscript_not_integer);
    return ExprError();
  }

  QualType pointee = pointer->GetType()->GetPointeeType();
  if (RequireCompleteType(rsquare, pointee,
                          diag::err_typecheck_incomplete_type_error)) {
    return ExprError();
  }

  return ctx_.New<ArraySubscriptExpr>(pointer, integer, pointee, rsquare);
}

ExprResult Sema::ActOnCallExpr(Expr* callee, std::vector<Expr*> args,
                               SourceLocation rparen) {
  if (!callee) return ExprError();

  ExprResult conv = DefaultFunctionArrayLvalueConversion(callee);
  if (conv.IsInvalid()) return ExprError();
  callee = conv.Get();

  QualType t = callee->GetType();
  const auto* pt = t.GetCanonical().GetTypePtr()->As<PointerType>();
  const FunctionType* fn_type =
      pt ? pt->GetPointee().GetCanonical().GetTypePtr()->As<FunctionType>()
         : nullptr;
  if (!fn_type) {
    Diag(callee->GetBeginLoc(), diag::err_typecheck_call_not_function)
        << t.GetAsString();
    return ExprError();
  }

  const auto* proto = fn_type->As<FunctionProtoType>();
  bool had_error = false;

  if (proto) {
    unsigned num_params = proto->GetNumParams();
    if (args.size() < num_params) {
      Diag(rparen, diag::err_typecheck_call_too_few_args)
          << num_params << static_cast<unsigned>(args.size());
      had_error = true;
    } else if (args.size() > num_params && !proto->IsVariadic()) {
      Diag(args[num_params]->GetBeginLoc(),
           diag::err_typecheck_call_too_many_args)
          << num_params << static_cast<unsigned>(args.size());
      had_error = true;
    }

    for (unsigned i = 0; i < args.size(); ++i) {
      if (i < num_params) {
        QualType param = proto->GetParamTypes()[i];
        Expr* arg = args[i];
        QualType arg_type = arg->GetType();
        AssignConvertType result =
            CheckSingleAssignmentConstraints(param, arg, &arg_type);
        DiagnoseAssignmentResult(result, args[i]->GetBeginLoc(), param,
                                 arg_type, "passing");
        if (result == AssignConvertType::kIncompatible) had_error = true;
        args[i] = arg;
      } else {
        ExprResult promoted = DefaultArgumentPromotion(args[i]);
        if (promoted.IsInvalid()) return ExprError();
        args[i] = promoted.Get();
      }
    }
  } else {
    for (Expr*& arg : args) {
      ExprResult promoted = DefaultArgumentPromotion(arg);
      if (promoted.IsInvalid()) return ExprError();
      arg = promoted.Get();
    }
  }

  std::vector<const Expr*> const_args(args.begin(), args.end());
  auto* call = ctx_.New<CallExpr>(callee, std::move(const_args),
                                  fn_type->GetReturnType(), rparen);
  if (had_error) call->SetContainsErrors();
  return call;
}

ExprResult Sema::ActOnMemberAccess(Expr* base, SourceLocation op_loc,
                                   bool is_arrow, const IdentifierInfo* name,
                                   SourceLocation name_loc) {
  if (!base) return ExprError();

  QualType base_type = base->GetType();
  const RecordType* record_type = nullptr;
  Qualifiers base_quals;

  if (is_arrow) {
    ExprResult conv = DefaultFunctionArrayLvalueConversion(base);
    if (conv.IsInvalid()) return ExprError();
    base = conv.Get();
    base_type = base->GetType();
    const auto* pt = base_type.GetCanonical().GetTypePtr()->As<PointerType>();
    if (!pt) {
      // `x.y` spelled `x->y` on a record is a common error worth special
      // casing? The reverse (`x.y` on pointer) is; here just diagnose.
      Diag(op_loc, diag::err_typecheck_member_reference_arrow)
          << base_type.GetAsString();
      return ExprError();
    }
    QualType pointee = pt->GetPointee().GetCanonical();
    record_type = pointee.GetTypePtr()->As<RecordType>();
    base_quals = pointee.GetQualifiers();
    if (!record_type) {
      Diag(op_loc, diag::err_typecheck_member_reference_struct_union)
          << pt->GetPointee().GetAsString();
      return ExprError();
    }
  } else {
    QualType canon = base_type.GetCanonical();
    record_type = canon.GetTypePtr()->As<RecordType>();
    base_quals = canon.GetQualifiers();
    if (!record_type) {
      if (canon.GetTypePtr()->As<PointerType>()) {
        Diag(op_loc, diag::err_typecheck_member_reference_suggestion)
            << base_type.GetAsString();
      } else {
        Diag(op_loc, diag::err_typecheck_member_reference_struct_union)
            << base_type.GetAsString();
      }
      return ExprError();
    }
  }

  const RecordDecl* record = record_type->GetDecl();
  if (!record->IsCompleteDefinition()) {
    Diag(op_loc, diag::err_typecheck_incomplete_tag)
        << QualType(record_type).GetAsString();
    return ExprError();
  }

  std::vector<const FieldDecl*> path;
  const FieldDecl* field = record->FindField(name, &path);
  if (!field) {
    Diag(name_loc, diag::err_no_member)
        << name->GetName() << QualType(record_type).GetAsString();
    return ExprError();
  }

  // Wrap anonymous-member hops in intermediate MemberExprs, then the found
  // field. Member qualifiers merge with the base's (C11 6.5.2.3p3-4).
  ValueKind vk =
      is_arrow || base->IsLValue() ? ValueKind::kLValue : ValueKind::kRValue;
  Expr* result = base;
  bool first = true;
  for (const FieldDecl* step : path) {
    QualType member_type = step->GetType().WithQualifiers(base_quals);
    result = ctx_.New<MemberExpr>(result, first && is_arrow, step, member_type,
                                  vk, name_loc);
    first = false;
  }
  return result;
}

//===----------------------------------------------------------------------===//
// Casts, sizeof, compound literals, _Generic.
//===----------------------------------------------------------------------===//

ExprResult Sema::ActOnCastExpr(SourceLocation lparen, QualType type,
                               SourceLocation rparen, Expr* operand) {
  (void)rparen;
  if (type.IsNull() || !operand) return ExprError();

  if (type->IsVoidType()) {
    ExprResult conv = DefaultFunctionArrayLvalueConversion(operand);
    if (conv.IsInvalid()) return ExprError();
    return ctx_.New<CStyleCastExpr>(CastKind::kToVoid, conv.Get(), type,
                                    lparen);
  }

  if (!type->IsScalarType()) {
    Diag(lparen, diag::err_typecheck_cond_expect_scalar) << type.GetAsString();
    return ExprError();
  }

  ExprResult conv = DefaultFunctionArrayLvalueConversion(operand);
  if (conv.IsInvalid()) return ExprError();
  operand = conv.Get();

  QualType from = operand->GetType();
  if (!from->IsScalarType()) {
    Diag(operand->GetBeginLoc(), diag::err_typecheck_cond_expect_scalar)
        << from.GetAsString();
    return ExprError();
  }

  // Pointer <-> floating is not a valid C cast.
  if ((from->IsPointerType() && type->IsFloatingType()) ||
      (from->IsFloatingType() && type->IsPointerType())) {
    Diag(lparen, diag::err_typecheck_cast_illegal)
        << from.GetAsString() << type.GetAsString();
    return ExprError();
  }

  CastKind kind = GetScalarCastKind(from, type);
  if (operand->IsNullPointerConstant() && type->IsPointerType() &&
      from->IsIntegerType()) {
    kind = CastKind::kNullToPointer;
  }
  return ctx_.New<CStyleCastExpr>(kind, operand, type, lparen);
}

ExprResult Sema::ActOnCompoundLiteral(SourceLocation lparen, QualType type,
                                      SourceLocation rparen, Expr* init) {
  (void)rparen;
  if (type.IsNull() || !init) return ExprError();

  bool is_file_scope = cur_function_ == nullptr;
  ExprResult checked = CheckInitializer(type, init, is_file_scope);
  if (checked.IsInvalid()) return ExprError();

  return ctx_.New<CompoundLiteralExpr>(type, checked.Get(), is_file_scope,
                                       lparen);
}

ExprResult Sema::ActOnSizeofAlignof(SourceLocation op_loc, bool is_sizeof,
                                    QualType type, Expr* operand,
                                    SourceRange range) {
  QualType query_type = type;
  if (operand) query_type = operand->GetType();
  if (query_type.IsNull()) return ExprError();

  QualType canon = query_type.GetCanonical();
  if (canon.GetTypePtr()->IsFunctionType()) {
    Diag(op_loc, diag::err_typecheck_sizeof_function);
    return ExprError();
  }
  if (canon.GetTypePtr()->As<VariableArrayType>()) {
    Diag(op_loc, diag::err_vla_unsupported);
    return ExprError();
  }
  if (!canon.GetTypePtr()->IsCompleteType()) {
    Diag(op_loc, is_sizeof ? diag::err_typecheck_sizeof_incomplete
                           : diag::err_typecheck_alignof_incomplete)
        << query_type.GetAsString();
    return ExprError();
  }

  uint64_t value =
      is_sizeof ? ctx_.GetTypeSize(query_type) : ctx_.GetTypeAlign(query_type);
  return ctx_.New<SizeOfAlignOfExpr>(is_sizeof, query_type, operand, value,
                                     ctx_.SizeTy(), range);
}

ExprResult Sema::ActOnGenericSelection(SourceLocation generic_loc,
                                       Expr* controlling,
                                       std::vector<GenericAssoc> assocs,
                                       SourceLocation rparen) {
  (void)rparen;
  if (!controlling) return ExprError();

  // The controlling expression undergoes lvalue conversion and array/function
  // decay for matching (matching Clang's C11 behavior).
  ExprResult conv = DefaultFunctionArrayLvalueConversion(controlling);
  if (conv.IsInvalid()) return ExprError();
  QualType control_type =
      conv.Get()->GetType().GetCanonical().WithoutQualifiers();

  // Validate: at most one default, no duplicate compatible types.
  const GenericAssoc* default_assoc = nullptr;
  for (std::size_t i = 0; i < assocs.size(); ++i) {
    if (assocs[i].type.IsNull()) {
      if (default_assoc) {
        Diag(assocs[i].loc, diag::err_generic_multiple_default);
        Diag(default_assoc->loc, diag::note_generic_prev_default);
        return ExprError();
      }
      default_assoc = &assocs[i];
      continue;
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (assocs[j].type.IsNull()) continue;
      if (ctx_.IsCompatible(assocs[i].type, assocs[j].type)) {
        Diag(assocs[i].loc, diag::err_generic_duplicate_match)
            << assocs[i].type.GetAsString() << assocs[j].type.GetAsString();
        Diag(assocs[j].loc, diag::note_generic_compat_type)
            << assocs[j].type.GetAsString();
        return ExprError();
      }
    }
  }

  const GenericAssoc* chosen = nullptr;
  for (const GenericAssoc& a : assocs) {
    if (a.type.IsNull()) continue;
    if (ctx_.IsCompatible(a.type.GetCanonical().WithoutQualifiers(),
                          control_type)) {
      chosen = &a;
      break;
    }
  }
  if (!chosen) chosen = default_assoc;
  if (!chosen) {
    Diag(generic_loc, diag::err_generic_no_match) << control_type.GetAsString();
    return ExprError();
  }
  if (!chosen->expr) return ExprError();

  return ctx_.New<GenericSelectionExpr>(
      controlling, chosen->expr,
      SourceRange{generic_loc, chosen->expr->GetEndLoc()});
}

ExprResult Sema::ActOnInitList(SourceLocation lbrace, std::vector<Expr*> inits,
                               SourceLocation rbrace) {
  std::vector<const Expr*> const_inits(inits.begin(), inits.end());
  return ctx_.New<InitListExpr>(std::move(const_inits),
                                SourceRange{lbrace, rbrace});
}

ExprResult Sema::ActOnDesignatedInit(std::vector<Designator> designators,
                                     Expr* init) {
  if (!init) return ExprError();
  return ctx_.New<DesignatedInitExpr>(std::move(designators), init);
}

}  // namespace bcc
