#include "bcc/sema/sema.hh"

namespace bcc {

namespace {

/// Wraps \p value to the representation of an integer type of \p width bits
/// with the given signedness (two's complement, matching x86-64).
int64_t Truncate(int64_t value, uint64_t width, bool is_unsigned) {
  if (width >= 64) return value;
  uint64_t mask = (uint64_t{1} << width) - 1;
  uint64_t v = static_cast<uint64_t>(value) & mask;
  if (!is_unsigned && (v & (uint64_t{1} << (width - 1)))) {
    v |= ~mask;  // sign-extend
  }
  return static_cast<int64_t>(v);
}

}  // namespace

std::optional<ICEValue> Sema::EvaluateICE(const Expr* e) const {
  if (!e || e->ContainsErrors()) return std::nullopt;

  QualType type = e->GetType();
  bool is_unsigned = !type.IsNull() && ctx_.IsUnsignedIntegerType(type);
  uint64_t width =
      !type.IsNull() && type->IsIntegerType() ? ctx_.GetIntWidth(type) : 64;

  auto wrap = [&](int64_t v) -> std::optional<ICEValue> {
    return ICEValue{Truncate(v, width, is_unsigned), is_unsigned};
  };

  switch (e->GetStmtClass()) {
    case StmtClass::kIntegerLiteral:
      return wrap(
          static_cast<int64_t>(static_cast<const IntegerLiteral*>(e)
                                   ->GetValue()));
    case StmtClass::kCharacterLiteral:
      return wrap(static_cast<const CharacterLiteral*>(e)->GetValue());
    case StmtClass::kSizeOfAlignOfExpr:
      return wrap(static_cast<int64_t>(
          static_cast<const SizeOfAlignOfExpr*>(e)->GetValue()));
    case StmtClass::kParenExpr:
      return EvaluateICE(static_cast<const ParenExpr*>(e)->GetSubExpr());
    case StmtClass::kGenericSelectionExpr:
      return EvaluateICE(
          static_cast<const GenericSelectionExpr*>(e)->GetChosenExpr());
    case StmtClass::kDeclRefExpr: {
      const auto* ref = static_cast<const DeclRefExpr*>(e);
      if (const auto* ec = ref->GetDecl()->As<EnumConstantDecl>()) {
        return wrap(ec->GetValue());
      }
      return std::nullopt;
    }

    case StmtClass::kImplicitCastExpr:
    case StmtClass::kCStyleCastExpr: {
      const auto* cast = static_cast<const CastExpr*>(e);
      const Expr* sub = cast->GetSubExpr();
      switch (cast->GetCastKind()) {
        case CastKind::kIntegralCast:
        case CastKind::kNoOp: {
          std::optional<ICEValue> v = EvaluateICE(sub);
          if (!v) return std::nullopt;
          return wrap(v->value);
        }
        case CastKind::kIntegralToBoolean: {
          std::optional<ICEValue> v = EvaluateICE(sub);
          if (!v) return std::nullopt;
          return wrap(v->value != 0 ? 1 : 0);
        }
        case CastKind::kFloatingToIntegral: {
          // C11 6.6p6 permits casting a floating *constant* to an integer.
          const Expr* inner = sub->IgnoreParenImpCasts();
          if (const auto* fl = inner->As<FloatingLiteral>()) {
            return wrap(static_cast<int64_t>(fl->GetValue()));
          }
          return std::nullopt;
        }
        default:
          return std::nullopt;
      }
    }

    case StmtClass::kUnaryOperator: {
      const auto* uo = static_cast<const UnaryOperator*>(e);
      std::optional<ICEValue> v = EvaluateICE(uo->GetSubExpr());
      if (!v) return std::nullopt;
      switch (uo->GetOpcode()) {
        case UnaryOperatorKind::kPlus: return wrap(v->value);
        case UnaryOperatorKind::kMinus: return wrap(-v->value);
        case UnaryOperatorKind::kNot: return wrap(~v->value);
        case UnaryOperatorKind::kLNot: return wrap(v->value == 0 ? 1 : 0);
        default: return std::nullopt;
      }
    }

    case StmtClass::kConditionalOperator: {
      const auto* co = static_cast<const ConditionalOperator*>(e);
      std::optional<ICEValue> cond = EvaluateICE(co->GetCond());
      if (!cond) return std::nullopt;
      return EvaluateICE(cond->value != 0 ? co->GetTrueExpr()
                                          : co->GetFalseExpr());
    }

    case StmtClass::kBinaryOperator: {
      const auto* bo = static_cast<const BinaryOperator*>(e);
      BinaryOperatorKind op = bo->GetOpcode();

      // Logical operators short-circuit: the unevaluated side need not be
      // constant.
      if (op == BinaryOperatorKind::kLAnd || op == BinaryOperatorKind::kLOr) {
        std::optional<ICEValue> lhs = EvaluateICE(bo->GetLHS());
        if (!lhs) return std::nullopt;
        bool lhs_true = lhs->value != 0;
        if (op == BinaryOperatorKind::kLAnd && !lhs_true) return wrap(0);
        if (op == BinaryOperatorKind::kLOr && lhs_true) return wrap(1);
        std::optional<ICEValue> rhs = EvaluateICE(bo->GetRHS());
        if (!rhs) return std::nullopt;
        return wrap(rhs->value != 0 ? 1 : 0);
      }

      std::optional<ICEValue> lhs = EvaluateICE(bo->GetLHS());
      std::optional<ICEValue> rhs = EvaluateICE(bo->GetRHS());
      if (!lhs || !rhs) return std::nullopt;
      int64_t l = lhs->value;
      int64_t r = rhs->value;

      // Comparisons of unsigned operands must compare unsigned.
      QualType lhs_type = bo->GetLHS()->GetType();
      bool operands_unsigned =
          !lhs_type.IsNull() && lhs_type->IsIntegerType() &&
          ctx_.IsUnsignedIntegerType(lhs_type);
      auto cmp = [&](auto pred) -> std::optional<ICEValue> {
        bool result = operands_unsigned
                          ? pred(static_cast<uint64_t>(l),
                                 static_cast<uint64_t>(r))
                          : pred(l, r);
        return wrap(result ? 1 : 0);
      };

      switch (op) {
        case BinaryOperatorKind::kMul:
          return wrap(is_unsigned
                          ? static_cast<int64_t>(static_cast<uint64_t>(l) *
                                                 static_cast<uint64_t>(r))
                          : l * r);
        case BinaryOperatorKind::kDiv:
          if (r == 0) return std::nullopt;
          if (is_unsigned) {
            return wrap(static_cast<int64_t>(static_cast<uint64_t>(l) /
                                             static_cast<uint64_t>(r)));
          }
          if (l == INT64_MIN && r == -1) return std::nullopt;
          return wrap(l / r);
        case BinaryOperatorKind::kRem:
          if (r == 0) return std::nullopt;
          if (is_unsigned) {
            return wrap(static_cast<int64_t>(static_cast<uint64_t>(l) %
                                             static_cast<uint64_t>(r)));
          }
          if (l == INT64_MIN && r == -1) return std::nullopt;
          return wrap(l % r);
        case BinaryOperatorKind::kAdd:
          return wrap(static_cast<int64_t>(static_cast<uint64_t>(l) +
                                           static_cast<uint64_t>(r)));
        case BinaryOperatorKind::kSub:
          return wrap(static_cast<int64_t>(static_cast<uint64_t>(l) -
                                           static_cast<uint64_t>(r)));
        case BinaryOperatorKind::kShl:
          if (r < 0 || static_cast<uint64_t>(r) >= width) return std::nullopt;
          return wrap(static_cast<int64_t>(static_cast<uint64_t>(l) << r));
        case BinaryOperatorKind::kShr:
          if (r < 0 || static_cast<uint64_t>(r) >= width) return std::nullopt;
          if (is_unsigned) {
            uint64_t mask =
                width >= 64 ? ~uint64_t{0} : (uint64_t{1} << width) - 1;
            return wrap(static_cast<int64_t>(
                (static_cast<uint64_t>(l) & mask) >> r));
          }
          return wrap(l >> r);
        case BinaryOperatorKind::kLT:
          return cmp([](auto a, auto b) { return a < b; });
        case BinaryOperatorKind::kGT:
          return cmp([](auto a, auto b) { return a > b; });
        case BinaryOperatorKind::kLE:
          return cmp([](auto a, auto b) { return a <= b; });
        case BinaryOperatorKind::kGE:
          return cmp([](auto a, auto b) { return a >= b; });
        case BinaryOperatorKind::kEQ:
          return wrap(l == r ? 1 : 0);
        case BinaryOperatorKind::kNE:
          return wrap(l != r ? 1 : 0);
        case BinaryOperatorKind::kAnd: return wrap(l & r);
        case BinaryOperatorKind::kXor: return wrap(l ^ r);
        case BinaryOperatorKind::kOr: return wrap(l | r);
        default:
          // Assignment and comma operators are not permitted in an ICE
          // (C11 6.6p3).
          return std::nullopt;
      }
    }

    default:
      return std::nullopt;
  }
}

std::optional<ICEValue> Sema::VerifyICE(const Expr* e, SourceLocation loc,
                                        diag::DiagKind kind) {
  std::optional<ICEValue> result = EvaluateICE(e);
  if (!result && e && !e->ContainsErrors()) Diag(loc, kind);
  return result;
}

}  // namespace bcc
