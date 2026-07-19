#include "bcc/ast/type.hh"

#include <string>

#include "bcc/ast/decl.hh"

namespace bcc {

namespace {

std::string QualPrefix(Qualifiers q) {
  std::string s;
  if (q.HasConst()) s += "const ";
  if (q.HasVolatile()) s += "volatile ";
  if (q.HasRestrict()) s += "restrict ";
  if (q.HasAtomic()) s += "_Atomic ";
  return s;
}

/// Renders \p t around the partial declarator \p inner, C-declarator style:
/// building `int (*)[4]` proceeds pointer -> array -> base type.
std::string PrintType(QualType t, std::string inner) {
  const Type* ty = t.GetTypePtr();
  Qualifiers quals = t.GetQualifiers();

  switch (ty->GetTypeClass()) {
    case TypeClass::kBuiltin: {
      std::string base =
          QualPrefix(quals) +
          std::string(static_cast<const BuiltinType*>(ty)->GetName());
      return inner.empty() ? base : base + " " + inner;
    }
    case TypeClass::kTypedef: {
      std::string base =
          QualPrefix(quals) +
          std::string(
              static_cast<const TypedefType*>(ty)->GetDecl()->GetName());
      return inner.empty() ? base : base + " " + inner;
    }
    case TypeClass::kRecord: {
      const RecordDecl* rd = static_cast<const RecordType*>(ty)->GetDecl();
      std::string base = QualPrefix(quals);
      base += rd->IsUnion() ? "union " : "struct ";
      base += rd->GetIdentifier() ? std::string(rd->GetName()) : "(anonymous)";
      return inner.empty() ? base : base + " " + inner;
    }
    case TypeClass::kEnum: {
      const EnumDecl* ed = static_cast<const EnumType*>(ty)->GetDecl();
      std::string base = QualPrefix(quals) + "enum ";
      base += ed->GetIdentifier() ? std::string(ed->GetName()) : "(anonymous)";
      return inner.empty() ? base : base + " " + inner;
    }
    case TypeClass::kPointer: {
      std::string s = "*";
      // Pointer qualifiers trail the '*': `int *const p`.
      if (!quals.IsEmpty()) {
        std::string q = QualPrefix(quals);
        q.pop_back();  // trailing space
        s += q;
        if (!inner.empty()) s += " ";
      }
      s += inner;
      QualType pointee = static_cast<const PointerType*>(ty)->GetPointee();
      TypeClass pc = pointee.GetTypePtr()->GetTypeClass();
      if (pc == TypeClass::kConstantArray || pc == TypeClass::kIncompleteArray ||
          pc == TypeClass::kVariableArray || pc == TypeClass::kFunctionProto ||
          pc == TypeClass::kFunctionNoProto) {
        s = "(" + s + ")";
      }
      return PrintType(pointee, std::move(s));
    }
    case TypeClass::kConstantArray: {
      const auto* at = static_cast<const ConstantArrayType*>(ty);
      inner += "[" + std::to_string(at->GetSize()) + "]";
      return PrintType(at->GetElementType().WithQualifiers(quals),
                       std::move(inner));
    }
    case TypeClass::kIncompleteArray: {
      inner += "[]";
      return PrintType(static_cast<const IncompleteArrayType*>(ty)
                           ->GetElementType()
                           .WithQualifiers(quals),
                       std::move(inner));
    }
    case TypeClass::kVariableArray: {
      inner += "[*]";
      return PrintType(static_cast<const VariableArrayType*>(ty)
                           ->GetElementType()
                           .WithQualifiers(quals),
                       std::move(inner));
    }
    case TypeClass::kFunctionProto: {
      const auto* ft = static_cast<const FunctionProtoType*>(ty);
      inner += "(";
      if (ft->GetNumParams() == 0 && !ft->IsVariadic()) {
        inner += "void";
      } else {
        bool first = true;
        for (QualType p : ft->GetParamTypes()) {
          if (!first) inner += ", ";
          first = false;
          inner += PrintType(p, "");
        }
        if (ft->IsVariadic()) inner += ", ...";
      }
      inner += ")";
      return PrintType(ft->GetReturnType(), std::move(inner));
    }
    case TypeClass::kFunctionNoProto: {
      inner += "()";
      return PrintType(
          static_cast<const FunctionNoProtoType*>(ty)->GetReturnType(),
          std::move(inner));
    }
  }
  return inner;
}

}  // namespace

QualType QualType::GetCanonical() const noexcept {
  if (IsNull()) return {};
  QualType canon = ty_->GetCanonicalType();
  return canon.WithQualifiers(quals_);
}

QualType QualType::Desugar() const noexcept {
  QualType t = *this;
  while (const auto* td = t.GetTypePtr()->As<TypedefType>()) {
    t = td->GetUnderlyingType().WithQualifiers(t.GetQualifiers());
  }
  return t;
}

std::string QualType::GetAsString() const {
  if (IsNull()) return "<null>";
  return PrintType(*this, "");
}

std::string_view BuiltinType::GetName() const noexcept {
  switch (kind_) {
    case BuiltinTypeKind::kVoid: return "void";
    case BuiltinTypeKind::kBool: return "_Bool";
    case BuiltinTypeKind::kChar: return "char";
    case BuiltinTypeKind::kSChar: return "signed char";
    case BuiltinTypeKind::kUChar: return "unsigned char";
    case BuiltinTypeKind::kShort: return "short";
    case BuiltinTypeKind::kUShort: return "unsigned short";
    case BuiltinTypeKind::kInt: return "int";
    case BuiltinTypeKind::kUInt: return "unsigned int";
    case BuiltinTypeKind::kLong: return "long";
    case BuiltinTypeKind::kULong: return "unsigned long";
    case BuiltinTypeKind::kLongLong: return "long long";
    case BuiltinTypeKind::kULongLong: return "unsigned long long";
    case BuiltinTypeKind::kFloat: return "float";
    case BuiltinTypeKind::kDouble: return "double";
    case BuiltinTypeKind::kLongDouble: return "long double";
  }
  return "<builtin>";
}

//===----------------------------------------------------------------------===//
// Predicates. All operate on the canonical type.
//===----------------------------------------------------------------------===//

bool Type::IsVoidType() const noexcept {
  const auto* bt = AsCanonical<BuiltinType>();
  return bt && bt->GetKind() == BuiltinTypeKind::kVoid;
}

bool Type::IsBoolType() const noexcept {
  const auto* bt = AsCanonical<BuiltinType>();
  return bt && bt->GetKind() == BuiltinTypeKind::kBool;
}

bool Type::IsIntegerType() const noexcept {
  if (const auto* bt = AsCanonical<BuiltinType>()) return bt->IsInteger();
  return AsCanonical<EnumType>() != nullptr;
}

bool Type::IsFloatingType() const noexcept {
  const auto* bt = AsCanonical<BuiltinType>();
  return bt && bt->IsFloating();
}

bool Type::IsPointerType() const noexcept {
  return AsCanonical<PointerType>() != nullptr;
}

bool Type::IsArrayType() const noexcept {
  return AsCanonical<ArrayType>() != nullptr;
}

bool Type::IsFunctionType() const noexcept {
  return AsCanonical<FunctionType>() != nullptr;
}

bool Type::IsRecordType() const noexcept {
  return AsCanonical<RecordType>() != nullptr;
}

bool Type::IsStructType() const noexcept {
  const auto* rt = AsCanonical<RecordType>();
  return rt && !rt->GetDecl()->IsUnion();
}

bool Type::IsUnionType() const noexcept {
  const auto* rt = AsCanonical<RecordType>();
  return rt && rt->GetDecl()->IsUnion();
}

bool Type::IsEnumType() const noexcept {
  return AsCanonical<EnumType>() != nullptr;
}

bool Type::IsCompleteType() const noexcept {
  const Type* canon = GetCanonicalType().GetTypePtr();
  switch (canon->GetTypeClass()) {
    case TypeClass::kBuiltin:
      return static_cast<const BuiltinType*>(canon)->GetKind() !=
             BuiltinTypeKind::kVoid;
    case TypeClass::kIncompleteArray:
      return false;
    case TypeClass::kConstantArray:
    case TypeClass::kVariableArray:
    case TypeClass::kPointer:
    // C treats function types as neither complete nor incomplete; callers
    // that need an object type test IsFunctionType() separately.
    case TypeClass::kFunctionProto:
    case TypeClass::kFunctionNoProto:
      return true;
    case TypeClass::kRecord:
      return static_cast<const RecordType*>(canon)
          ->GetDecl()
          ->IsCompleteDefinition();
    case TypeClass::kEnum:
      return static_cast<const EnumType*>(canon)
          ->GetDecl()
          ->IsCompleteDefinition();
    case TypeClass::kTypedef:
      break;  // canonical types are never sugar
  }
  return false;
}

QualType Type::GetPointeeType() const noexcept {
  if (const auto* pt = AsCanonical<PointerType>()) return pt->GetPointee();
  return {};
}

QualType Type::GetArrayElementType() const noexcept {
  if (const auto* at = AsCanonical<ArrayType>()) return at->GetElementType();
  return {};
}

}  // namespace bcc
