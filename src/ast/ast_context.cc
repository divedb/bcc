#include "bcc/ast/ast_context.hh"

#include <algorithm>
#include <cassert>

#include "bcc/ast/expr.hh"
#include "bcc/ast/stmt.hh"

namespace bcc {

ASTContext::ASTContext() {
  InitBuiltinTypes();
  tu_ = New<TranslationUnitDecl>();
}

ASTContext::~ASTContext() = default;

void ASTContext::InitBuiltinTypes() {
  for (unsigned i = 0; i <= static_cast<unsigned>(BuiltinTypeKind::kLongDouble);
       ++i) {
    auto t = std::make_unique<BuiltinType>(static_cast<BuiltinTypeKind>(i));
    BuiltinType* raw = t.get();
    raw->SetCanonicalType(QualType(raw));
    types_.push_back(std::move(t));
    builtins_[i] = QualType(raw);
  }
}

const Type* ASTContext::Own(std::unique_ptr<Type> t, QualType canonical) {
  Type* raw = t.get();
  raw->SetCanonicalType(canonical.IsNull() ? QualType(raw) : canonical);
  types_.push_back(std::move(t));
  return raw;
}

//===----------------------------------------------------------------------===//
// Type construction.
//===----------------------------------------------------------------------===//

QualType ASTContext::GetPointerType(QualType pointee) {
  auto [it, inserted] = pointer_types_.try_emplace(pointee, nullptr);
  if (inserted) {
    QualType canon;
    QualType canon_pointee = pointee.GetCanonical();
    if (canon_pointee != pointee) canon = GetPointerType(canon_pointee);
    it->second = Own(std::make_unique<PointerType>(pointee), canon);
  }
  return QualType(it->second);
}

QualType ASTContext::GetConstantArrayType(QualType element, uint64_t size) {
  auto [it, inserted] =
      constant_array_types_.try_emplace({element, size}, nullptr);
  if (inserted) {
    QualType canon;
    QualType canon_elem = element.GetCanonical();
    if (canon_elem != element) canon = GetConstantArrayType(canon_elem, size);
    it->second = Own(std::make_unique<ConstantArrayType>(element, size), canon);
  }
  return QualType(it->second);
}

QualType ASTContext::GetIncompleteArrayType(QualType element) {
  auto [it, inserted] = incomplete_array_types_.try_emplace(element, nullptr);
  if (inserted) {
    QualType canon;
    QualType canon_elem = element.GetCanonical();
    if (canon_elem != element) canon = GetIncompleteArrayType(canon_elem);
    it->second = Own(std::make_unique<IncompleteArrayType>(element), canon);
  }
  return QualType(it->second);
}

QualType ASTContext::GetVariableArrayType(QualType element,
                                          const Expr* size_expr) {
  // VLA types are not uniqued: each size expression is distinct.
  return QualType(
      Own(std::make_unique<VariableArrayType>(element, size_expr), {}));
}

QualType ASTContext::GetFunctionType(QualType ret, std::vector<QualType> params,
                                     bool is_variadic) {
  FunctionTypeKey key{ret, params, is_variadic, /*no_proto=*/false};
  auto [it, inserted] = function_types_.try_emplace(std::move(key), nullptr);
  if (inserted) {
    QualType canon;
    QualType canon_ret = ret.GetCanonical();
    std::vector<QualType> canon_params;
    canon_params.reserve(params.size());
    bool all_canonical = canon_ret == ret;
    for (QualType p : params) {
      // Canonical parameter types drop top-level qualifiers (C11 6.7.6.3p15).
      QualType cp = p.GetCanonical().WithoutQualifiers();
      if (cp != p) all_canonical = false;
      canon_params.push_back(cp);
    }
    if (!all_canonical) {
      canon = GetFunctionType(canon_ret, std::move(canon_params), is_variadic);
    }
    it->second = Own(std::make_unique<FunctionProtoType>(
                         ret, std::move(params), is_variadic),
                     canon);
  }
  return QualType(it->second);
}

QualType ASTContext::GetFunctionNoProtoType(QualType ret) {
  FunctionTypeKey key{ret, {}, /*variadic=*/false, /*no_proto=*/true};
  auto [it, inserted] = function_types_.try_emplace(std::move(key), nullptr);
  if (inserted) {
    QualType canon;
    QualType canon_ret = ret.GetCanonical();
    if (canon_ret != ret) canon = GetFunctionNoProtoType(canon_ret);
    it->second = Own(std::make_unique<FunctionNoProtoType>(ret), canon);
  }
  return QualType(it->second);
}

QualType ASTContext::GetTagType(const TagDecl* tag) {
  auto [it, inserted] = tag_types_.try_emplace(tag, nullptr);
  if (inserted) {
    if (const auto* rd = tag->As<RecordDecl>()) {
      it->second = Own(std::make_unique<RecordType>(rd), {});
    } else {
      it->second = Own(std::make_unique<EnumType>(tag->As<EnumDecl>()), {});
    }
  }
  return QualType(it->second);
}

QualType ASTContext::GetTypedefType(const TypedefDecl* decl) {
  auto [it, inserted] = typedef_types_.try_emplace(decl, nullptr);
  if (inserted) {
    QualType underlying = decl->GetType();
    it->second = Own(std::make_unique<TypedefType>(decl, underlying),
                     underlying.GetCanonical());
  }
  return QualType(it->second);
}

QualType ASTContext::GetStringLiteralArrayType(QualType element,
                                               uint64_t len) {
  return GetConstantArrayType(element, len + 1);
}

//===----------------------------------------------------------------------===//
// Compatibility and composite types (C11 6.2.7).
//===----------------------------------------------------------------------===//

bool ASTContext::IsCompatibleCanonical(QualType a, QualType b) {
  if (a.GetTypePtr() == b.GetTypePtr()) {
    return a.GetQualifiers() == b.GetQualifiers();
  }
  if (a.GetQualifiers() != b.GetQualifiers()) return false;

  const Type* at = a.GetTypePtr();
  const Type* bt = b.GetTypePtr();

  // An enum type is compatible with its implementation-defined underlying
  // integer type; bcc uses int (C11 6.7.2.2p4).
  auto is_int = [](const Type* t) {
    const auto* builtin = t->As<BuiltinType>();
    return builtin && builtin->GetKind() == BuiltinTypeKind::kInt;
  };
  if ((at->As<EnumType>() && is_int(bt)) ||
      (bt->As<EnumType>() && is_int(at))) {
    return true;
  }

  if (at->GetTypeClass() != bt->GetTypeClass()) {
    // Constant vs incomplete arrays may still be compatible.
    const auto* aa = at->As<ArrayType>();
    const auto* ba = bt->As<ArrayType>();
    if (aa && ba) {
      return IsCompatibleCanonical(aa->GetElementType().GetCanonical(),
                                   ba->GetElementType().GetCanonical());
    }
    // Prototyped vs unprototyped functions (C11 6.7.6.3p15).
    const auto* af = at->As<FunctionType>();
    const auto* bf = bt->As<FunctionType>();
    if (af && bf) {
      if (!IsCompatibleCanonical(af->GetReturnType().GetCanonical(),
                                 bf->GetReturnType().GetCanonical())) {
        return false;
      }
      const auto* proto = at->As<FunctionProtoType>();
      if (!proto) proto = bt->As<FunctionProtoType>();
      if (proto->IsVariadic()) return false;
      for (QualType p : proto->GetParamTypes()) {
        // Each parameter type must be unaffected by the default argument
        // promotions.
        const auto* builtin = p.GetCanonical().GetTypePtr()->As<BuiltinType>();
        if (builtin) {
          switch (builtin->GetKind()) {
            case BuiltinTypeKind::kBool:
            case BuiltinTypeKind::kChar:
            case BuiltinTypeKind::kSChar:
            case BuiltinTypeKind::kUChar:
            case BuiltinTypeKind::kShort:
            case BuiltinTypeKind::kUShort:
            case BuiltinTypeKind::kFloat:
              return false;
            default:
              break;
          }
        }
      }
      return true;
    }
    return false;
  }

  switch (at->GetTypeClass()) {
    case TypeClass::kBuiltin:
      return static_cast<const BuiltinType*>(at)->GetKind() ==
             static_cast<const BuiltinType*>(bt)->GetKind();
    case TypeClass::kPointer:
      return IsCompatibleCanonical(
          static_cast<const PointerType*>(at)->GetPointee().GetCanonical(),
          static_cast<const PointerType*>(bt)->GetPointee().GetCanonical());
    case TypeClass::kConstantArray: {
      const auto* aa = static_cast<const ConstantArrayType*>(at);
      const auto* ba = static_cast<const ConstantArrayType*>(bt);
      return aa->GetSize() == ba->GetSize() &&
             IsCompatibleCanonical(aa->GetElementType().GetCanonical(),
                                   ba->GetElementType().GetCanonical());
    }
    case TypeClass::kIncompleteArray:
    case TypeClass::kVariableArray:
      return IsCompatibleCanonical(
          static_cast<const ArrayType*>(at)->GetElementType().GetCanonical(),
          static_cast<const ArrayType*>(bt)->GetElementType().GetCanonical());
    case TypeClass::kFunctionProto: {
      const auto* af = static_cast<const FunctionProtoType*>(at);
      const auto* bf = static_cast<const FunctionProtoType*>(bt);
      if (af->IsVariadic() != bf->IsVariadic()) return false;
      if (af->GetNumParams() != bf->GetNumParams()) return false;
      if (!IsCompatibleCanonical(af->GetReturnType().GetCanonical(),
                                 bf->GetReturnType().GetCanonical())) {
        return false;
      }
      for (unsigned i = 0; i < af->GetNumParams(); ++i) {
        if (!IsCompatibleCanonical(
                af->GetParamTypes()[i].GetCanonical().WithoutQualifiers(),
                bf->GetParamTypes()[i].GetCanonical().WithoutQualifiers())) {
          return false;
        }
      }
      return true;
    }
    case TypeClass::kFunctionNoProto:
      return IsCompatibleCanonical(
          static_cast<const FunctionNoProtoType*>(at)
              ->GetReturnType()
              .GetCanonical(),
          static_cast<const FunctionNoProtoType*>(bt)
              ->GetReturnType()
              .GetCanonical());
    case TypeClass::kRecord:
    case TypeClass::kEnum:
      return false;  // distinct tag decls are incompatible
    case TypeClass::kTypedef:
      break;  // canonical types are never sugar
  }
  return false;
}

bool ASTContext::IsCompatible(QualType a, QualType b) const {
  return IsCompatibleCanonical(a.GetCanonical(), b.GetCanonical());
}

QualType ASTContext::GetCompositeType(QualType a, QualType b) {
  if (!IsCompatible(a, b)) return {};
  QualType ca = a.GetCanonical();
  QualType cb = b.GetCanonical();
  const Type* at = ca.GetTypePtr();
  const Type* bt = cb.GetTypePtr();

  // Composite of array types: a known bound wins.
  if (at->As<ArrayType>() && bt->As<ArrayType>()) {
    if (bt->As<ConstantArrayType>() && !at->As<ConstantArrayType>()) return b;
    return a;
  }
  // Composite of function types: a prototype wins.
  if (at->As<FunctionType>() && bt->As<FunctionType>()) {
    if (bt->As<FunctionProtoType>() && !at->As<FunctionProtoType>()) return b;
    return a;
  }
  if (at->As<PointerType>() && bt->As<PointerType>()) {
    QualType pointee = GetCompositeType(
        static_cast<const PointerType*>(at)->GetPointee(),
        static_cast<const PointerType*>(bt)->GetPointee());
    return GetPointerType(pointee).WithQualifiers(ca.GetQualifiers());
  }
  return a;
}

QualType ASTContext::GetDecayedType(QualType t) {
  QualType canon = t.GetCanonical();
  if (const auto* at = canon.GetTypePtr()->As<ArrayType>()) {
    return GetPointerType(at->GetElementType());
  }
  if (canon.GetTypePtr()->As<FunctionType>()) {
    return GetPointerType(t.WithoutQualifiers());
  }
  return t;
}

//===----------------------------------------------------------------------===//
// Integer classification and promotion (C11 6.3.1.1).
//===----------------------------------------------------------------------===//

int ASTContext::GetIntegerRank(QualType t) const {
  QualType canon = t.GetCanonical();
  if (canon.GetTypePtr()->As<EnumType>()) return 4;  // ranks as int
  const auto* bt = canon.GetTypePtr()->As<BuiltinType>();
  assert(bt && bt->IsInteger() && "not an integer type");
  switch (bt->GetKind()) {
    case BuiltinTypeKind::kBool: return 1;
    case BuiltinTypeKind::kChar:
    case BuiltinTypeKind::kSChar:
    case BuiltinTypeKind::kUChar: return 2;
    case BuiltinTypeKind::kShort:
    case BuiltinTypeKind::kUShort: return 3;
    case BuiltinTypeKind::kInt:
    case BuiltinTypeKind::kUInt: return 4;
    case BuiltinTypeKind::kLong:
    case BuiltinTypeKind::kULong: return 5;
    case BuiltinTypeKind::kLongLong:
    case BuiltinTypeKind::kULongLong: return 6;
    default: return 0;
  }
}

bool ASTContext::IsSignedIntegerType(QualType t) const {
  QualType canon = t.GetCanonical();
  if (canon.GetTypePtr()->As<EnumType>()) return true;  // underlying int
  const auto* bt = canon.GetTypePtr()->As<BuiltinType>();
  return bt && bt->IsSignedInteger();
}

bool ASTContext::IsUnsignedIntegerType(QualType t) const {
  QualType canon = t.GetCanonical();
  const auto* bt = canon.GetTypePtr()->As<BuiltinType>();
  return bt && bt->IsUnsignedInteger();
}

QualType ASTContext::GetCorrespondingUnsignedType(QualType t) const {
  const auto* bt = t.GetCanonical().GetTypePtr()->As<BuiltinType>();
  if (!bt) return UIntTy();  // enum -> unsigned int
  switch (bt->GetKind()) {
    case BuiltinTypeKind::kChar:
    case BuiltinTypeKind::kSChar: return UCharTy();
    case BuiltinTypeKind::kShort: return UShortTy();
    case BuiltinTypeKind::kInt: return UIntTy();
    case BuiltinTypeKind::kLong: return ULongTy();
    case BuiltinTypeKind::kLongLong: return ULongLongTy();
    default: return t;
  }
}

QualType ASTContext::GetPromotedIntegerType(QualType t) const {
  assert(t->IsIntegerType());
  // On x86-64 every type of rank < int fits in int, so promotion is int.
  if (GetIntegerRank(t) < GetIntegerRank(IntTy())) return IntTy();
  if (t.GetCanonical().GetTypePtr()->As<EnumType>()) return IntTy();
  return t.WithoutQualifiers();
}

//===----------------------------------------------------------------------===//
// Layout (x86-64 SysV).
//===----------------------------------------------------------------------===//

namespace {

uint64_t BuiltinSize(BuiltinTypeKind kind) {
  switch (kind) {
    case BuiltinTypeKind::kVoid: return 1;  // GNU compat for void arith
    case BuiltinTypeKind::kBool:
    case BuiltinTypeKind::kChar:
    case BuiltinTypeKind::kSChar:
    case BuiltinTypeKind::kUChar: return 1;
    case BuiltinTypeKind::kShort:
    case BuiltinTypeKind::kUShort: return 2;
    case BuiltinTypeKind::kInt:
    case BuiltinTypeKind::kUInt:
    case BuiltinTypeKind::kFloat: return 4;
    case BuiltinTypeKind::kLong:
    case BuiltinTypeKind::kULong:
    case BuiltinTypeKind::kLongLong:
    case BuiltinTypeKind::kULongLong:
    case BuiltinTypeKind::kDouble: return 8;
    case BuiltinTypeKind::kLongDouble: return 16;
  }
  return 1;
}

uint64_t RoundUp(uint64_t v, uint64_t align) {
  return (v + align - 1) / align * align;
}

}  // namespace

uint64_t ASTContext::GetTypeSize(QualType t) const {
  const Type* canon = t.GetCanonical().GetTypePtr();
  switch (canon->GetTypeClass()) {
    case TypeClass::kBuiltin:
      return BuiltinSize(static_cast<const BuiltinType*>(canon)->GetKind());
    case TypeClass::kPointer:
      return 8;
    case TypeClass::kEnum:
      return 4;
    case TypeClass::kConstantArray: {
      const auto* at = static_cast<const ConstantArrayType*>(canon);
      return at->GetSize() * GetTypeSize(at->GetElementType());
    }
    case TypeClass::kRecord:
      return GetRecordLayout(static_cast<const RecordType*>(canon)->GetDecl())
          .size;
    default:
      assert(false && "GetTypeSize on incomplete/function/VLA type");
      return 1;
  }
}

uint64_t ASTContext::GetTypeAlign(QualType t) const {
  const Type* canon = t.GetCanonical().GetTypePtr();
  switch (canon->GetTypeClass()) {
    case TypeClass::kBuiltin:
      return BuiltinSize(static_cast<const BuiltinType*>(canon)->GetKind());
    case TypeClass::kPointer:
      return 8;
    case TypeClass::kEnum:
      return 4;
    case TypeClass::kConstantArray:
    case TypeClass::kIncompleteArray:
    case TypeClass::kVariableArray:
      return GetTypeAlign(
          static_cast<const ArrayType*>(canon)->GetElementType());
    case TypeClass::kRecord:
      return GetRecordLayout(static_cast<const RecordType*>(canon)->GetDecl())
          .align;
    default:
      assert(false && "GetTypeAlign on function type");
      return 1;
  }
}

const RecordLayout& ASTContext::GetRecordLayout(
    const RecordDecl* record) const {
  auto it = record_layouts_.find(record);
  if (it != record_layouts_.end()) return it->second;

  assert(record->IsCompleteDefinition() && "layout of incomplete record");

  RecordLayout layout;
  uint64_t bits = 0;      // running struct size / max union size, in bits
  uint64_t max_bits = 0;  // union: largest member

  for (const FieldDecl* field : record->GetFields()) {
    QualType ft = field->GetType();
    uint64_t field_align_bits;
    uint64_t field_bits;

    if (field->IsBitField()) {
      uint64_t type_bits = GetTypeSize(ft) * 8;
      field_align_bits = GetTypeAlign(ft) * 8;
      unsigned width = field->GetBitWidth();
      if (width == 0) {
        // Zero-width bit-field: align the next member to the unit boundary;
        // does not affect the struct's alignment (C11 6.7.2.1p12).
        bits = RoundUp(bits, field_align_bits);
        layout.field_offsets_bits.push_back(bits);
        continue;
      }
      if (record->IsUnion()) {
        layout.field_offsets_bits.push_back(0);
        max_bits = std::max<uint64_t>(max_bits, type_bits);
      } else {
        // A bit-field may not cross an allocation-unit boundary of its type.
        if (bits % type_bits + width > type_bits) {
          bits = RoundUp(bits, type_bits);
        }
        layout.field_offsets_bits.push_back(bits);
        bits += width;
      }
      layout.align = std::max(layout.align, field_align_bits / 8);
      continue;
    }

    // Flexible array member: aligns the struct but occupies no space.
    bool flexible = ft.GetCanonical().GetTypePtr()->As<IncompleteArrayType>();
    field_align_bits = GetTypeAlign(ft) * 8;
    field_bits = flexible ? 0 : GetTypeSize(ft) * 8;

    if (record->IsUnion()) {
      layout.field_offsets_bits.push_back(0);
      max_bits = std::max(max_bits, field_bits);
    } else {
      bits = RoundUp(bits, field_align_bits);
      layout.field_offsets_bits.push_back(bits);
      bits += field_bits;
    }
    layout.align = std::max(layout.align, field_align_bits / 8);
  }

  uint64_t total = record->IsUnion() ? max_bits : bits;
  layout.size = RoundUp(total, layout.align * 8) / 8;
  // An empty struct still has size 0 in C (GNU) — Clang gives it size 0 too.

  return record_layouts_.emplace(record, std::move(layout)).first->second;
}

}  // namespace bcc
