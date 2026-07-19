// Folds static-storage initializers to ir::Constants: literals, enum
// constants, arithmetic and casts over constants, sizeof, address constants
// (&global, functions, string literals, array decay), init lists (padding
// zero-filled), strings. Deliberately re-implements the constant-address
// subset of Clang's ConstantEmitter instead of reusing Sema's ICE evaluator
// (which is integer-only and Sema-internal).
//
// Known v1 limits (reported as err_codegen_cannot_compile by the caller):
// address constants with non-zero offsets (&arr[3], &s.b), non-zero
// bit-field initializers, and union members that need pointer serialization.

#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "bcc/codegen/codegen_module.hh"

namespace bcc::codegen {

namespace {

class ConstantEmitter {
 public:
  explicit ConstantEmitter(CodeGenModule& cgm)
      : cgm_(cgm), ast_(cgm.GetASTContext()), ir_(cgm.GetIRContext()) {}

  const ir::Constant* Emit(const Expr* e, QualType dest_type);

 private:
  std::optional<int64_t> FoldInt(const Expr* e);
  std::optional<double> FoldFP(const Expr* e);
  const ir::Constant* FoldPointer(const Expr* e);
  const ir::Constant* EmitArray(const Expr* e, QualType type);
  const ir::Constant* EmitRecord(const InitListExpr* e, QualType type);
  const ir::Constant* EmitStringForArray(const StringLiteral* s,
                                         QualType array_type);
  const ir::Constant* NullOf(const ir::Type* t);
  /// Serializes an already-folded constant into little-endian bytes (for
  /// union initializers); false if it contains a relocation (address).
  bool SerializeToBytes(const ir::Constant* c, const ir::Type* t,
                        std::string& out, size_t offset);

  /// Normalizes a raw value to \p t's width/signedness (as sign-extended
  /// int64 storage).
  int64_t Normalize(int64_t v, QualType t) {
    uint64_t bits = ast_.GetIntWidth(t);
    if (bits >= 64) return v;
    uint64_t mask = (uint64_t{1} << bits) - 1;
    uint64_t u = static_cast<uint64_t>(v) & mask;
    if (ast_.IsSignedIntegerType(t) && (u & (uint64_t{1} << (bits - 1)))) {
      u |= ~mask;
    }
    return static_cast<int64_t>(u);
  }

  CodeGenModule& cgm_;
  ASTContext& ast_;
  ir::IRContext& ir_;
};

const ir::Constant* ConstantEmitter::Emit(const Expr* e, QualType dest_type) {
  if (!e) return cgm_.EmitNullConstant(dest_type);
  const Type* canon = dest_type.GetCanonical().GetTypePtr();

  if (canon->IsIntegerType()) {
    std::optional<int64_t> v = FoldInt(e);
    if (!v) return nullptr;
    const auto* it =
        cgm_.GetTypes().Convert(dest_type)->As<ir::IntegerType>();
    return ir_.GetInt(it, static_cast<uint64_t>(*v));
  }
  if (canon->IsFloatingType()) {
    std::optional<double> v = FoldFP(e);
    if (!v) return nullptr;
    const ir::Type* t = cgm_.GetTypes().Convert(dest_type);
    return ir_.GetFP(t, t->IsFloat() ? static_cast<float>(*v) : *v);
  }
  if (canon->IsPointerType()) return FoldPointer(e);
  if (canon->IsArrayType()) return EmitArray(e, dest_type);
  if (canon->IsRecordType()) {
    const Expr* stripped = e->IgnoreParens();
    if (const auto* ile = stripped->As<InitListExpr>()) {
      return EmitRecord(ile, dest_type);
    }
    return nullptr;
  }
  return nullptr;
}

std::optional<int64_t> ConstantEmitter::FoldInt(const Expr* e) {
  switch (e->GetStmtClass()) {
    case StmtClass::kIntegerLiteral:
      return Normalize(
          static_cast<int64_t>(e->As<IntegerLiteral>()->GetValue()),
          e->GetType());
    case StmtClass::kCharacterLiteral:
      return static_cast<int64_t>(e->As<CharacterLiteral>()->GetValue());
    case StmtClass::kSizeOfAlignOfExpr:
      return static_cast<int64_t>(e->As<SizeOfAlignOfExpr>()->GetValue());
    case StmtClass::kDeclRefExpr: {
      const auto* ec =
          e->As<DeclRefExpr>()->GetDecl()->As<EnumConstantDecl>();
      if (ec) return ec->GetValue();
      return std::nullopt;
    }
    case StmtClass::kParenExpr:
      return FoldInt(e->As<ParenExpr>()->GetSubExpr());
    case StmtClass::kGenericSelectionExpr:
      return FoldInt(e->As<GenericSelectionExpr>()->GetChosenExpr());
    case StmtClass::kUnaryOperator: {
      const auto* uo = e->As<UnaryOperator>();
      std::optional<int64_t> v = FoldInt(uo->GetSubExpr());
      if (!v) return std::nullopt;
      switch (uo->GetOpcode()) {
        case UnaryOperatorKind::kPlus: return v;
        case UnaryOperatorKind::kMinus: return Normalize(-*v, e->GetType());
        case UnaryOperatorKind::kNot: return Normalize(~*v, e->GetType());
        case UnaryOperatorKind::kLNot: return *v == 0 ? 1 : 0;
        default: return std::nullopt;
      }
    }
    case StmtClass::kBinaryOperator: {
      const auto* bo = e->As<BinaryOperator>();
      // Short-circuit forms first (RHS may be non-constant but unevaluated).
      if (bo->GetOpcode() == BinaryOperatorKind::kLAnd ||
          bo->GetOpcode() == BinaryOperatorKind::kLOr) {
        std::optional<int64_t> l = FoldInt(bo->GetLHS());
        std::optional<int64_t> r = FoldInt(bo->GetRHS());
        if (!l || !r) return std::nullopt;
        return bo->GetOpcode() == BinaryOperatorKind::kLAnd
                   ? (*l != 0 && *r != 0)
                   : (*l != 0 || *r != 0);
      }
      std::optional<int64_t> l = FoldInt(bo->GetLHS());
      std::optional<int64_t> r = FoldInt(bo->GetRHS());
      if (!l || !r) return std::nullopt;
      QualType t = bo->GetLHS()->GetType();
      bool is_signed = ast_.IsSignedIntegerType(t);
      uint64_t ul = static_cast<uint64_t>(*l), ur = static_cast<uint64_t>(*r);
      switch (bo->GetOpcode()) {
        case BinaryOperatorKind::kMul:
          return Normalize(*l * *r, e->GetType());
        case BinaryOperatorKind::kDiv:
          if (*r == 0) return std::nullopt;
          return Normalize(is_signed ? *l / *r
                                     : static_cast<int64_t>(ul / ur),
                           e->GetType());
        case BinaryOperatorKind::kRem:
          if (*r == 0) return std::nullopt;
          return Normalize(is_signed ? *l % *r
                                     : static_cast<int64_t>(ul % ur),
                           e->GetType());
        case BinaryOperatorKind::kAdd:
          return Normalize(*l + *r, e->GetType());
        case BinaryOperatorKind::kSub:
          return Normalize(*l - *r, e->GetType());
        case BinaryOperatorKind::kShl:
          return Normalize(static_cast<int64_t>(ul << (ur & 63)),
                           e->GetType());
        case BinaryOperatorKind::kShr:
          return Normalize(is_signed
                               ? *l >> (ur & 63)
                               : static_cast<int64_t>(ul >> (ur & 63)),
                           e->GetType());
        case BinaryOperatorKind::kLT:
          return is_signed ? *l < *r : ul < ur;
        case BinaryOperatorKind::kGT:
          return is_signed ? *l > *r : ul > ur;
        case BinaryOperatorKind::kLE:
          return is_signed ? *l <= *r : ul <= ur;
        case BinaryOperatorKind::kGE:
          return is_signed ? *l >= *r : ul >= ur;
        case BinaryOperatorKind::kEQ:
          return *l == *r;
        case BinaryOperatorKind::kNE:
          return *l != *r;
        case BinaryOperatorKind::kAnd:
          return Normalize(*l & *r, e->GetType());
        case BinaryOperatorKind::kXor:
          return Normalize(*l ^ *r, e->GetType());
        case BinaryOperatorKind::kOr:
          return Normalize(*l | *r, e->GetType());
        case BinaryOperatorKind::kComma:
          return r;
        default:
          return std::nullopt;
      }
    }
    case StmtClass::kConditionalOperator: {
      const auto* co = e->As<ConditionalOperator>();
      std::optional<int64_t> c = FoldInt(co->GetCond());
      if (!c) return std::nullopt;
      return FoldInt(*c ? co->GetTrueExpr() : co->GetFalseExpr());
    }
    case StmtClass::kImplicitCastExpr:
    case StmtClass::kCStyleCastExpr: {
      const auto* ce = e->As<CastExpr>();
      const Expr* sub = ce->GetSubExpr();
      switch (ce->GetCastKind()) {
        case CastKind::kIntegralCast: {
          std::optional<int64_t> v = FoldInt(sub);
          if (!v) return std::nullopt;
          return Normalize(*v, e->GetType());
        }
        case CastKind::kIntegralToBoolean:
          if (std::optional<int64_t> v = FoldInt(sub)) return *v != 0;
          return std::nullopt;
        case CastKind::kFloatingToBoolean:
          if (std::optional<double> v = FoldFP(sub)) return *v != 0.0;
          return std::nullopt;
        case CastKind::kFloatingToIntegral: {
          std::optional<double> v = FoldFP(sub);
          if (!v) return std::nullopt;
          return Normalize(ast_.IsSignedIntegerType(e->GetType())
                               ? static_cast<int64_t>(*v)
                               : static_cast<int64_t>(
                                     static_cast<uint64_t>(*v)),
                           e->GetType());
        }
        case CastKind::kNoOp:
          return FoldInt(sub);
        default:
          return std::nullopt;
      }
    }
    default:
      return std::nullopt;
  }
}

std::optional<double> ConstantEmitter::FoldFP(const Expr* e) {
  switch (e->GetStmtClass()) {
    case StmtClass::kFloatingLiteral:
      return e->As<FloatingLiteral>()->GetValue();
    case StmtClass::kParenExpr:
      return FoldFP(e->As<ParenExpr>()->GetSubExpr());
    case StmtClass::kGenericSelectionExpr:
      return FoldFP(e->As<GenericSelectionExpr>()->GetChosenExpr());
    case StmtClass::kUnaryOperator: {
      const auto* uo = e->As<UnaryOperator>();
      std::optional<double> v = FoldFP(uo->GetSubExpr());
      if (!v) return std::nullopt;
      switch (uo->GetOpcode()) {
        case UnaryOperatorKind::kPlus: return v;
        case UnaryOperatorKind::kMinus: return -*v;
        default: return std::nullopt;
      }
    }
    case StmtClass::kBinaryOperator: {
      const auto* bo = e->As<BinaryOperator>();
      std::optional<double> l = FoldFP(bo->GetLHS());
      std::optional<double> r = FoldFP(bo->GetRHS());
      if (!l || !r) return std::nullopt;
      switch (bo->GetOpcode()) {
        case BinaryOperatorKind::kMul: return *l * *r;
        case BinaryOperatorKind::kDiv: return *l / *r;
        case BinaryOperatorKind::kAdd: return *l + *r;
        case BinaryOperatorKind::kSub: return *l - *r;
        default: return std::nullopt;
      }
    }
    case StmtClass::kImplicitCastExpr:
    case StmtClass::kCStyleCastExpr: {
      const auto* ce = e->As<CastExpr>();
      const Expr* sub = ce->GetSubExpr();
      switch (ce->GetCastKind()) {
        case CastKind::kIntegralToFloating: {
          std::optional<int64_t> v = FoldInt(sub);
          if (!v) return std::nullopt;
          if (ast_.IsSignedIntegerType(sub->GetType())) {
            return static_cast<double>(*v);
          }
          return static_cast<double>(static_cast<uint64_t>(*v));
        }
        case CastKind::kFloatingCast: {
          std::optional<double> v = FoldFP(sub);
          if (!v) return std::nullopt;
          // Narrowing through float must round like the runtime would.
          if (ast_.GetTypeSize(e->GetType()) == 4) {
            return static_cast<double>(static_cast<float>(*v));
          }
          return v;
        }
        case CastKind::kNoOp:
          return FoldFP(sub);
        default:
          return std::nullopt;
      }
    }
    default:
      return std::nullopt;
  }
}

const ir::Constant* ConstantEmitter::FoldPointer(const Expr* e) {
  switch (e->GetStmtClass()) {
    case StmtClass::kParenExpr:
      return FoldPointer(e->As<ParenExpr>()->GetSubExpr());
    case StmtClass::kGenericSelectionExpr:
      return FoldPointer(e->As<GenericSelectionExpr>()->GetChosenExpr());
    case StmtClass::kImplicitCastExpr:
    case StmtClass::kCStyleCastExpr: {
      const auto* ce = e->As<CastExpr>();
      const Expr* sub = ce->GetSubExpr();
      switch (ce->GetCastKind()) {
        case CastKind::kNullToPointer:
          return ir_.GetNullPtr();
        case CastKind::kBitCast:
        case CastKind::kNoOp:
          return FoldPointer(sub);
        case CastKind::kArrayToPointerDecay: {
          const Expr* stripped = sub->IgnoreParens();
          if (const auto* sl = stripped->As<StringLiteral>()) {
            return cgm_.GetStringLiteral(sl);
          }
          if (const auto* dre = stripped->As<DeclRefExpr>()) {
            if (const auto* vd = dre->GetDecl()->As<VarDecl>()) {
              if (vd->HasStaticStorage()) return cgm_.GetOrCreateGlobal(vd);
            }
          }
          return nullptr;
        }
        case CastKind::kFunctionToPointerDecay: {
          const Expr* stripped = sub->IgnoreParens();
          if (const auto* dre = stripped->As<DeclRefExpr>()) {
            if (const auto* fd = dre->GetDecl()->As<FunctionDecl>()) {
              ir::Function* fn = cgm_.GetOrCreateFunction(fd);
              fn->SetUsed();
              return fn;
            }
          }
          return nullptr;
        }
        case CastKind::kIntegralToPointer: {
          std::optional<int64_t> v = FoldInt(sub);
          if (v && *v == 0) return ir_.GetNullPtr();
          return nullptr;  // no inttoptr constant exprs in this IR
        }
        default:
          return nullptr;
      }
    }
    case StmtClass::kUnaryOperator: {
      const auto* uo = e->As<UnaryOperator>();
      if (uo->GetOpcode() != UnaryOperatorKind::kAddrOf) return nullptr;
      const Expr* sub = uo->GetSubExpr()->IgnoreParens();
      if (const auto* dre = sub->As<DeclRefExpr>()) {
        if (const auto* vd = dre->GetDecl()->As<VarDecl>()) {
          if (vd->HasStaticStorage()) return cgm_.GetOrCreateGlobal(vd);
        }
        if (const auto* fd = dre->GetDecl()->As<FunctionDecl>()) {
          ir::Function* fn = cgm_.GetOrCreateFunction(fd);
          fn->SetUsed();
          return fn;
        }
      }
      return nullptr;  // &arr[k], &s.f: constant offsets not supported yet
    }
    default:
      return nullptr;
  }
}

const ir::Constant* ConstantEmitter::NullOf(const ir::Type* t) {
  if (const auto* it = t->As<ir::IntegerType>()) return ir_.GetInt(it, 0);
  if (t->IsFloatingPoint()) return ir_.GetFP(t, 0.0);
  if (t->IsPointer()) return ir_.GetNullPtr();
  return ir_.GetAggregateZero(t);
}

const ir::Constant* ConstantEmitter::EmitStringForArray(
    const StringLiteral* s, QualType array_type) {
  const auto* at = array_type.GetCanonical().GetTypePtr()
                       ->As<ConstantArrayType>();
  uint64_t n = at ? at->GetSize() : s->GetLength() + 1;
  unsigned width = s->GetCharByteWidth();
  std::string_view bytes = s->GetBytes();

  if (width == 1) {
    std::string padded(bytes.substr(0, std::min<uint64_t>(bytes.size(), n)));
    padded.resize(n, '\0');
    return ir_.GetString(std::move(padded));
  }

  // Wide strings: [N x iW] element list.
  const ir::Type* elem = cgm_.GetTypes().Convert(at->GetElementType());
  const auto* elem_int = elem->As<ir::IntegerType>();
  std::vector<const ir::Constant*> elems;
  elems.reserve(n);
  for (uint64_t i = 0; i < n; ++i) {
    uint64_t v = 0;
    if ((i + 1) * width <= bytes.size()) {
      for (unsigned b = 0; b < width; ++b) {
        v |= static_cast<uint64_t>(
                 static_cast<unsigned char>(bytes[i * width + b]))
             << (8 * b);
      }
    }
    elems.push_back(ir_.GetInt(elem_int, v));
  }
  return ir_.GetAggregate(ir_.GetArrayType(elem, n), std::move(elems));
}

const ir::Constant* ConstantEmitter::EmitArray(const Expr* e,
                                               QualType type) {
  const Expr* stripped = e->IgnoreParens();
  if (const auto* sl = stripped->As<StringLiteral>()) {
    return EmitStringForArray(sl, type);
  }
  const auto* ile = stripped->As<InitListExpr>();
  if (!ile) return nullptr;

  const auto* at =
      type.GetCanonical().GetTypePtr()->As<ConstantArrayType>();
  if (!at) return nullptr;
  QualType elem_t = at->GetElementType();
  const ir::Type* ir_elem = cgm_.GetTypes().Convert(elem_t);
  uint64_t n = at->GetSize();

  const auto& inits = ile->GetInits();
  std::vector<const ir::Constant*> elems(n, nullptr);
  bool all_zero = true;
  for (uint64_t i = 0; i < n; ++i) {
    const Expr* init = i < inits.size() ? inits[i] : nullptr;
    const ir::Constant* c =
        init ? Emit(init, elem_t) : NullOf(ir_elem);
    if (!c) return nullptr;
    if (c != NullOf(ir_elem)) all_zero = false;
    elems[i] = c;
  }
  const ir::Type* arr = ir_.GetArrayType(ir_elem, n);
  if (all_zero) return ir_.GetAggregateZero(arr);
  return ir_.GetAggregate(arr, std::move(elems));
}

const ir::Constant* ConstantEmitter::EmitRecord(const InitListExpr* e,
                                                QualType type) {
  const auto* rt = type.GetCanonical().GetTypePtr()->As<RecordType>();
  if (!rt) return nullptr;
  const RecordDecl* rd = rt->GetDecl();
  const CodeGenTypes::RecordInfo& info = cgm_.GetTypes().GetRecordInfo(rd);
  const auto& fields = rd->GetFields();
  const auto& inits = e->GetInits();

  if (rd->IsUnion()) {
    // Serialize the (single) initialized member into the byte blanket.
    uint64_t size = info.type->GetSize();
    std::string bytes(size, '\0');
    const FieldDecl* field = e->GetInitializedField();
    const Expr* init = inits.empty() ? nullptr : inits[0];
    if (field && init) {
      if (field->IsBitField()) {
        std::optional<int64_t> v = FoldInt(init);
        if (!v) return nullptr;
        if (*v != 0) return nullptr;  // packed bit patterns: v1 limit
      } else {
        const ir::Constant* c = Emit(init, field->GetType());
        if (!c) return nullptr;
        if (!SerializeToBytes(c, cgm_.GetTypes().Convert(field->GetType()),
                              bytes, 0)) {
          return nullptr;
        }
      }
    }
    bool all_zero = bytes.find_first_not_of('\0') == std::string::npos;
    if (all_zero) return ir_.GetAggregateZero(info.type);
    return ir_.GetAggregate(info.type,
                            {ir_.GetString(std::move(bytes))});
  }

  const auto& ir_fields = info.type->GetFields();
  std::vector<const ir::Constant*> elems(ir_fields.size(), nullptr);
  for (size_t i = 0; i < ir_fields.size(); ++i) elems[i] = NullOf(ir_fields[i]);

  bool all_zero = true;
  for (size_t i = 0; i < inits.size() && i < fields.size(); ++i) {
    const Expr* init = inits[i];
    if (!init) continue;
    const FieldDecl* field = fields[i];
    if (field->IsBitField()) {
      std::optional<int64_t> v = FoldInt(init);
      if (!v) return nullptr;
      if (*v != 0) return nullptr;  // packed bit patterns: v1 limit
      continue;
    }
    auto idx = info.field_index.find(field);
    if (idx == info.field_index.end()) continue;
    const ir::Constant* c = Emit(init, field->GetType());
    if (!c) return nullptr;
    if (c != NullOf(ir_fields[idx->second])) all_zero = false;
    elems[idx->second] = c;
  }

  if (all_zero) return ir_.GetAggregateZero(info.type);
  return ir_.GetAggregate(info.type, std::move(elems));
}

bool ConstantEmitter::SerializeToBytes(const ir::Constant* c,
                                       const ir::Type* t, std::string& out,
                                       size_t offset) {
  if (c->As<ir::ConstantAggregateZero>()) return true;  // already zero
  if (const auto* ci = c->As<ir::ConstantInt>()) {
    uint64_t v = ci->GetValue();
    unsigned bytes = ci->GetType()->As<ir::IntegerType>()->GetBits() / 8;
    for (unsigned b = 0; b < bytes && offset + b < out.size(); ++b) {
      out[offset + b] = static_cast<char>((v >> (8 * b)) & 0xff);
    }
    return true;
  }
  if (const auto* cf = c->As<ir::ConstantFP>()) {
    if (cf->GetType()->IsFloat()) {
      float f = static_cast<float>(cf->GetValue());
      uint32_t v;
      std::memcpy(&v, &f, 4);
      for (unsigned b = 0; b < 4 && offset + b < out.size(); ++b) {
        out[offset + b] = static_cast<char>((v >> (8 * b)) & 0xff);
      }
    } else {
      double d = cf->GetValue();
      uint64_t v;
      std::memcpy(&v, &d, 8);
      for (unsigned b = 0; b < 8 && offset + b < out.size(); ++b) {
        out[offset + b] = static_cast<char>((v >> (8 * b)) & 0xff);
      }
    }
    return true;
  }
  if (c->As<ir::ConstantNullPtr>()) return true;  // zeros
  if (const auto* cs = c->As<ir::ConstantString>()) {
    const std::string& bytes = cs->GetBytes();
    for (size_t b = 0; b < bytes.size() && offset + b < out.size(); ++b) {
      out[offset + b] = bytes[b];
    }
    return true;
  }
  if (const auto* agg = c->As<ir::ConstantAggregate>()) {
    if (const auto* at = t->As<ir::ArrayType>()) {
      uint64_t elem_size = 0;
      // Element sizes are derivable only for scalars/nested arrays we
      // serialize; compute from the IR type.
      const ir::Type* et = at->GetElementType();
      if (const auto* it = et->As<ir::IntegerType>()) {
        elem_size = it->GetBits() / 8;
      } else if (et->IsFloat()) {
        elem_size = 4;
      } else if (et->IsDouble() || et->IsPointer()) {
        elem_size = 8;
      } else {
        return false;
      }
      const auto& elems = agg->GetElements();
      for (size_t i = 0; i < elems.size(); ++i) {
        if (!SerializeToBytes(elems[i], et, out, offset + i * elem_size)) {
          return false;
        }
      }
      return true;
    }
    return false;  // nested structs in unions: v1 limit
  }
  return false;  // addresses need relocations, not bytes
}

}  // namespace

const ir::Constant* CodeGenModule::EmitConstantInit(const Expr* init,
                                                    QualType type) {
  return ConstantEmitter(*this).Emit(init, type);
}

}  // namespace bcc::codegen
