#include "bcc/ast/expr.hh"

#include "bcc/ast/decl.hh"

namespace bcc {

std::string_view GetCastKindName(CastKind kind) noexcept {
  switch (kind) {
    case CastKind::kLValueToRValue: return "LValueToRValue";
    case CastKind::kArrayToPointerDecay: return "ArrayToPointerDecay";
    case CastKind::kFunctionToPointerDecay: return "FunctionToPointerDecay";
    case CastKind::kIntegralCast: return "IntegralCast";
    case CastKind::kIntegralToFloating: return "IntegralToFloating";
    case CastKind::kFloatingToIntegral: return "FloatingToIntegral";
    case CastKind::kFloatingCast: return "FloatingCast";
    case CastKind::kIntegralToBoolean: return "IntegralToBoolean";
    case CastKind::kFloatingToBoolean: return "FloatingToBoolean";
    case CastKind::kPointerToBoolean: return "PointerToBoolean";
    case CastKind::kIntegralToPointer: return "IntegralToPointer";
    case CastKind::kPointerToIntegral: return "PointerToIntegral";
    case CastKind::kNullToPointer: return "NullToPointer";
    case CastKind::kBitCast: return "BitCast";
    case CastKind::kToVoid: return "ToVoid";
    case CastKind::kNoOp: return "NoOp";
  }
  return "<cast>";
}

std::string_view GetUnaryOperatorSpelling(UnaryOperatorKind op) noexcept {
  switch (op) {
    case UnaryOperatorKind::kPostInc:
    case UnaryOperatorKind::kPreInc: return "++";
    case UnaryOperatorKind::kPostDec:
    case UnaryOperatorKind::kPreDec: return "--";
    case UnaryOperatorKind::kAddrOf: return "&";
    case UnaryOperatorKind::kDeref: return "*";
    case UnaryOperatorKind::kPlus: return "+";
    case UnaryOperatorKind::kMinus: return "-";
    case UnaryOperatorKind::kNot: return "~";
    case UnaryOperatorKind::kLNot: return "!";
  }
  return "";
}

std::string_view GetBinaryOperatorSpelling(BinaryOperatorKind op) noexcept {
  switch (op) {
    case BinaryOperatorKind::kMul: return "*";
    case BinaryOperatorKind::kDiv: return "/";
    case BinaryOperatorKind::kRem: return "%";
    case BinaryOperatorKind::kAdd: return "+";
    case BinaryOperatorKind::kSub: return "-";
    case BinaryOperatorKind::kShl: return "<<";
    case BinaryOperatorKind::kShr: return ">>";
    case BinaryOperatorKind::kLT: return "<";
    case BinaryOperatorKind::kGT: return ">";
    case BinaryOperatorKind::kLE: return "<=";
    case BinaryOperatorKind::kGE: return ">=";
    case BinaryOperatorKind::kEQ: return "==";
    case BinaryOperatorKind::kNE: return "!=";
    case BinaryOperatorKind::kAnd: return "&";
    case BinaryOperatorKind::kXor: return "^";
    case BinaryOperatorKind::kOr: return "|";
    case BinaryOperatorKind::kLAnd: return "&&";
    case BinaryOperatorKind::kLOr: return "||";
    case BinaryOperatorKind::kAssign: return "=";
    case BinaryOperatorKind::kMulAssign: return "*=";
    case BinaryOperatorKind::kDivAssign: return "/=";
    case BinaryOperatorKind::kRemAssign: return "%=";
    case BinaryOperatorKind::kAddAssign: return "+=";
    case BinaryOperatorKind::kSubAssign: return "-=";
    case BinaryOperatorKind::kShlAssign: return "<<=";
    case BinaryOperatorKind::kShrAssign: return ">>=";
    case BinaryOperatorKind::kAndAssign: return "&=";
    case BinaryOperatorKind::kXorAssign: return "^=";
    case BinaryOperatorKind::kOrAssign: return "|=";
    case BinaryOperatorKind::kComma: return ",";
  }
  return "";
}

const Expr* Expr::IgnoreParens() const noexcept {
  const Expr* e = this;
  while (const auto* pe = e->As<ParenExpr>()) e = pe->GetSubExpr();
  return e;
}

const Expr* Expr::IgnoreParenImpCasts() const noexcept {
  const Expr* e = this;
  for (;;) {
    if (const auto* pe = e->As<ParenExpr>()) {
      e = pe->GetSubExpr();
    } else if (const auto* ice = e->As<ImplicitCastExpr>()) {
      e = ice->GetSubExpr();
    } else {
      return e;
    }
  }
}

bool Expr::IsNullPointerConstant() const noexcept {
  const Expr* e = IgnoreParens();
  // (void*)0 is a null pointer constant.
  if (const auto* cast = e->As<CStyleCastExpr>()) {
    QualType t = cast->GetType().GetCanonical();
    if (const auto* pt = t.GetTypePtr()->As<PointerType>()) {
      QualType pointee = pt->GetPointee();
      if (pointee.GetTypePtr()->IsVoidType() &&
          pointee.GetQualifiers().IsEmpty()) {
        return cast->GetSubExpr()->IsNullPointerConstant();
      }
    }
    return false;
  }
  if (const auto* cast = e->As<ImplicitCastExpr>()) {
    return cast->GetSubExpr()->IsNullPointerConstant();
  }
  if (const auto* lit = e->As<IntegerLiteral>()) return lit->GetValue() == 0;
  if (const auto* lit = e->As<CharacterLiteral>()) return lit->GetValue() == 0;
  if (const auto* ref = e->As<DeclRefExpr>()) {
    if (const auto* ec = ref->GetDecl()->As<EnumConstantDecl>()) {
      return ec->GetValue() == 0;
    }
  }
  return false;
}

namespace {

/// True if \p t or any member of it (recursively) is const-qualified.
bool HasConstComponent(QualType t) {
  QualType canon = t.GetCanonical();
  if (canon.GetQualifiers().HasConst()) return true;
  if (const auto* rt = canon.GetTypePtr()->As<RecordType>()) {
    for (const FieldDecl* f : rt->GetDecl()->GetFields()) {
      if (HasConstComponent(f->GetType())) return true;
    }
  }
  return false;
}

}  // namespace

bool Expr::IsModifiableLValue() const noexcept {
  if (!IsLValue()) return false;
  QualType t = GetType();
  if (t.IsNull()) return false;
  if (t->IsArrayType()) return false;
  if (!t->IsCompleteType()) return false;
  return !HasConstComponent(t);
}

}  // namespace bcc
