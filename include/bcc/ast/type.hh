#pragma once

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace bcc {

class ASTContext;
class EnumDecl;
class Expr;
class RecordDecl;
class Type;
class TypedefDecl;

/// \brief The C type qualifiers (C11 6.7.3), stored as a bitmask.
class Qualifiers {
 public:
  enum : uint8_t {
    kNone = 0,
    kConst = 0x1,
    kVolatile = 0x2,
    kRestrict = 0x4,
    kAtomic = 0x8,
  };

  Qualifiers() = default;
  explicit Qualifiers(uint8_t mask) : mask_(mask) {}

  bool HasConst() const noexcept { return mask_ & kConst; }
  bool HasVolatile() const noexcept { return mask_ & kVolatile; }
  bool HasRestrict() const noexcept { return mask_ & kRestrict; }
  bool HasAtomic() const noexcept { return mask_ & kAtomic; }
  bool IsEmpty() const noexcept { return mask_ == 0; }
  uint8_t GetMask() const noexcept { return mask_; }

  void Add(uint8_t bits) noexcept { mask_ |= bits; }

  /// \brief True if this qualifier set is a superset of \p other, i.e. an
  ///        assignment from \p other-qualified to this-qualified pointee does
  ///        not discard qualifiers (C11 6.5.16.1p1).
  bool Contains(Qualifiers other) const noexcept {
    return (mask_ & other.mask_) == other.mask_;
  }

  bool operator==(const Qualifiers&) const = default;

 private:
  uint8_t mask_ = 0;
};

/// \brief A Type pointer plus its qualifiers — the unit the compiler passes
///        around for "a type" (mirrors Clang's QualType).
class QualType {
 public:
  QualType() = default;
  explicit QualType(const Type* ty, Qualifiers quals = {})
      : ty_(ty), quals_(quals) {}

  const Type* GetTypePtr() const noexcept { return ty_; }
  Qualifiers GetQualifiers() const noexcept { return quals_; }
  bool IsNull() const noexcept { return ty_ == nullptr; }

  const Type* operator->() const noexcept { return ty_; }

  QualType WithQualifiers(Qualifiers q) const noexcept {
    Qualifiers merged = quals_;
    merged.Add(q.GetMask());
    return QualType(ty_, merged);
  }

  QualType WithConst() const noexcept {
    return WithQualifiers(Qualifiers(Qualifiers::kConst));
  }

  QualType WithoutQualifiers() const noexcept { return QualType(ty_); }

  bool HasConst() const noexcept { return quals_.HasConst(); }
  bool HasVolatile() const noexcept { return quals_.HasVolatile(); }

  /// \brief The canonical (fully desugared) type, with this level's qualifiers
  ///        merged into the canonical qualifiers.
  QualType GetCanonical() const noexcept;

  /// \brief Strips top-level typedef sugar only (keeps qualifiers).
  QualType Desugar() const noexcept;

  /// \brief Identity comparison (same type pointer and qualifiers); use
  ///        ASTContext for compatibility queries.
  bool operator==(const QualType&) const = default;

  /// Total order for use as a uniquing-map key.
  bool operator<(const QualType& o) const noexcept {
    if (ty_ != o.ty_) return ty_ < o.ty_;
    return quals_.GetMask() < o.quals_.GetMask();
  }

  /// \brief Renders this type in C syntax (e.g. "const int *(*)[4]").
  std::string GetAsString() const;

 private:
  const Type* ty_ = nullptr;
  Qualifiers quals_;
};

enum class TypeClass : uint8_t {
  kBuiltin,
  kPointer,
  kConstantArray,
  kIncompleteArray,
  kVariableArray,
  kFunctionProto,
  kFunctionNoProto,
  kRecord,
  kEnum,
  kTypedef,
};

/// \brief Base of the C type hierarchy. All Type nodes are created and uniqued
///        by the ASTContext; type equality on canonical types is pointer
///        equality.
class Type {
 public:
  virtual ~Type() = default;

  Type(const Type&) = delete;
  Type& operator=(const Type&) = delete;

  TypeClass GetTypeClass() const noexcept { return tc_; }

  /// \brief The canonical type this (possibly sugared) type resolves to.
  ///        For non-sugar types this is the type itself (no qualifiers).
  QualType GetCanonicalType() const noexcept { return canonical_; }
  bool IsCanonical() const noexcept { return canonical_.GetTypePtr() == this; }

  template <typename T>
  const T* As() const noexcept {
    return T::ClassOf(this) ? static_cast<const T*>(this) : nullptr;
  }

  /// \brief Like As(), but looks through typedef sugar first.
  template <typename T>
  const T* AsCanonical() const noexcept {
    return canonical_.GetTypePtr()->As<T>();
  }

  //===--------------------------------------------------------------------===//
  // Predicates (C11 6.2.5). All look through typedef sugar.
  //===--------------------------------------------------------------------===//

  bool IsVoidType() const noexcept;
  bool IsBoolType() const noexcept;
  /// Integer types: char, signed/unsigned integers, _Bool, enums.
  bool IsIntegerType() const noexcept;
  bool IsFloatingType() const noexcept;
  bool IsRealType() const noexcept { return IsArithmeticType(); }
  bool IsArithmeticType() const noexcept {
    return IsIntegerType() || IsFloatingType();
  }
  bool IsPointerType() const noexcept;
  bool IsScalarType() const noexcept {
    return IsArithmeticType() || IsPointerType();
  }
  bool IsArrayType() const noexcept;
  bool IsFunctionType() const noexcept;
  bool IsRecordType() const noexcept;  // struct or union
  bool IsStructType() const noexcept;
  bool IsUnionType() const noexcept;
  bool IsEnumType() const noexcept;
  bool IsAggregateType() const noexcept {  // array or struct (C 6.2.5p21)
    return IsArrayType() || IsStructType();
  }

  /// \brief True for types with known size: everything but void, incomplete
  ///        arrays, and undefined struct/union/enum (C11 6.2.5p1).
  bool IsCompleteType() const noexcept;

  /// \brief The pointee if this is (sugar for) a pointer type, else null.
  QualType GetPointeeType() const noexcept;

  /// \brief The element type if this is (sugar for) an array, else null.
  QualType GetArrayElementType() const noexcept;

 protected:
  Type(TypeClass tc) : tc_(tc) {}

  /// Set once by ASTContext immediately after construction.
  void SetCanonicalType(QualType canonical) noexcept { canonical_ = canonical; }

 private:
  friend class ASTContext;

  TypeClass tc_;
  QualType canonical_;
};

enum class BuiltinTypeKind : uint8_t {
  kVoid,
  kBool,
  kChar,  // plain char (signed on x86-64, but a distinct type)
  kSChar,
  kUChar,
  kShort,
  kUShort,
  kInt,
  kUInt,
  kLong,
  kULong,
  kLongLong,
  kULongLong,
  kFloat,
  kDouble,
  kLongDouble,
};

class BuiltinType final : public Type {
 public:
  explicit BuiltinType(BuiltinTypeKind kind)
      : Type(TypeClass::kBuiltin), kind_(kind) {}

  BuiltinTypeKind GetKind() const noexcept { return kind_; }

  bool IsInteger() const noexcept {
    return kind_ >= BuiltinTypeKind::kBool &&
           kind_ <= BuiltinTypeKind::kULongLong;
  }

  bool IsFloating() const noexcept {
    return kind_ >= BuiltinTypeKind::kFloat;
  }

  bool IsSignedInteger() const noexcept {
    switch (kind_) {
      case BuiltinTypeKind::kChar:
      case BuiltinTypeKind::kSChar:
      case BuiltinTypeKind::kShort:
      case BuiltinTypeKind::kInt:
      case BuiltinTypeKind::kLong:
      case BuiltinTypeKind::kLongLong:
        return true;
      default:
        return false;
    }
  }

  bool IsUnsignedInteger() const noexcept {
    return IsInteger() && !IsSignedInteger();
  }

  std::string_view GetName() const noexcept;

  static bool ClassOf(const Type* t) noexcept {
    return t->GetTypeClass() == TypeClass::kBuiltin;
  }

 private:
  BuiltinTypeKind kind_;
};

class PointerType final : public Type {
 public:
  explicit PointerType(QualType pointee)
      : Type(TypeClass::kPointer), pointee_(pointee) {}

  QualType GetPointee() const noexcept { return pointee_; }

  static bool ClassOf(const Type* t) noexcept {
    return t->GetTypeClass() == TypeClass::kPointer;
  }

 private:
  QualType pointee_;
};

class ArrayType : public Type {
 public:
  QualType GetElementType() const noexcept { return element_; }

  static bool ClassOf(const Type* t) noexcept {
    return t->GetTypeClass() == TypeClass::kConstantArray ||
           t->GetTypeClass() == TypeClass::kIncompleteArray ||
           t->GetTypeClass() == TypeClass::kVariableArray;
  }

 protected:
  ArrayType(TypeClass tc, QualType element) : Type(tc), element_(element) {}

 private:
  QualType element_;
};

/// Array with a known constant bound: `T[N]`.
class ConstantArrayType final : public ArrayType {
 public:
  ConstantArrayType(QualType element, uint64_t size)
      : ArrayType(TypeClass::kConstantArray, element), size_(size) {}

  uint64_t GetSize() const noexcept { return size_; }

  static bool ClassOf(const Type* t) noexcept {
    return t->GetTypeClass() == TypeClass::kConstantArray;
  }

 private:
  uint64_t size_;
};

/// Array of unknown bound: `T[]`.
class IncompleteArrayType final : public ArrayType {
 public:
  explicit IncompleteArrayType(QualType element)
      : ArrayType(TypeClass::kIncompleteArray, element) {}

  static bool ClassOf(const Type* t) noexcept {
    return t->GetTypeClass() == TypeClass::kIncompleteArray;
  }
};

/// Variable-length array: `T[n]` with a non-constant bound. Represented but
/// not semantically evaluated (sizeof of a VLA is diagnosed).
class VariableArrayType final : public ArrayType {
 public:
  VariableArrayType(QualType element, const Expr* size_expr)
      : ArrayType(TypeClass::kVariableArray, element), size_expr_(size_expr) {}

  const Expr* GetSizeExpr() const noexcept { return size_expr_; }

  static bool ClassOf(const Type* t) noexcept {
    return t->GetTypeClass() == TypeClass::kVariableArray;
  }

 private:
  const Expr* size_expr_;
};

class FunctionType : public Type {
 public:
  QualType GetReturnType() const noexcept { return return_type_; }

  static bool ClassOf(const Type* t) noexcept {
    return t->GetTypeClass() == TypeClass::kFunctionProto ||
           t->GetTypeClass() == TypeClass::kFunctionNoProto;
  }

 protected:
  FunctionType(TypeClass tc, QualType return_type)
      : Type(tc), return_type_(return_type) {}

 private:
  QualType return_type_;
};

/// `T(void)` / `T(int, char*)` / `T(int, ...)` — a function with a prototype.
class FunctionProtoType final : public FunctionType {
 public:
  FunctionProtoType(QualType return_type, std::vector<QualType> params,
                    bool is_variadic)
      : FunctionType(TypeClass::kFunctionProto, return_type),
        params_(std::move(params)),
        is_variadic_(is_variadic) {}

  const std::vector<QualType>& GetParamTypes() const noexcept {
    return params_;
  }
  unsigned GetNumParams() const noexcept {
    return static_cast<unsigned>(params_.size());
  }
  bool IsVariadic() const noexcept { return is_variadic_; }

  static bool ClassOf(const Type* t) noexcept {
    return t->GetTypeClass() == TypeClass::kFunctionProto;
  }

 private:
  std::vector<QualType> params_;
  bool is_variadic_;
};

/// `T()` — a function declared with an empty (unprototyped) parameter list.
class FunctionNoProtoType final : public FunctionType {
 public:
  explicit FunctionNoProtoType(QualType return_type)
      : FunctionType(TypeClass::kFunctionNoProto, return_type) {}

  static bool ClassOf(const Type* t) noexcept {
    return t->GetTypeClass() == TypeClass::kFunctionNoProto;
  }
};

/// A struct or union type; identity is the RecordDecl that declared it.
class RecordType final : public Type {
 public:
  explicit RecordType(const RecordDecl* decl)
      : Type(TypeClass::kRecord), decl_(decl) {}

  const RecordDecl* GetDecl() const noexcept { return decl_; }

  static bool ClassOf(const Type* t) noexcept {
    return t->GetTypeClass() == TypeClass::kRecord;
  }

 private:
  const RecordDecl* decl_;
};

class EnumType final : public Type {
 public:
  explicit EnumType(const EnumDecl* decl)
      : Type(TypeClass::kEnum), decl_(decl) {}

  const EnumDecl* GetDecl() const noexcept { return decl_; }

  static bool ClassOf(const Type* t) noexcept {
    return t->GetTypeClass() == TypeClass::kEnum;
  }

 private:
  const EnumDecl* decl_;
};

/// Sugar for a typedef-name use; canonicalizes to the underlying type.
class TypedefType final : public Type {
 public:
  TypedefType(const TypedefDecl* decl, QualType underlying)
      : Type(TypeClass::kTypedef), decl_(decl), underlying_(underlying) {}

  const TypedefDecl* GetDecl() const noexcept { return decl_; }
  QualType GetUnderlyingType() const noexcept { return underlying_; }

  static bool ClassOf(const Type* t) noexcept {
    return t->GetTypeClass() == TypeClass::kTypedef;
  }

 private:
  const TypedefDecl* decl_;
  QualType underlying_;
};

}  // namespace bcc
