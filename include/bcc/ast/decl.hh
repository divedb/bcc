#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "bcc/ast/type.hh"
#include "bcc/basic/source_location.hh"

namespace bcc {

class CompoundStmt;
class Expr;
class IdentifierInfo;
class LabelStmt;
class Stmt;

enum class DeclKind : uint8_t {
  kTranslationUnit,
  kTypedef,
  kVar,
  kParmVar,
  kFunction,
  kField,
  kRecord,
  kEnum,
  kEnumConstant,
  kLabel,
  kStaticAssert,
};

/// \brief C storage-class specifiers as written (C11 6.7.1). kNone means no
///        storage class was written; typedef is a DeclKind, not a storage
///        class, by the time Sema builds a Decl.
enum class StorageClass : uint8_t {
  kNone,
  kExtern,
  kStatic,
  kAuto,
  kRegister,
};

/// \brief Base of the declaration hierarchy. All Decl nodes are owned by the
///        ASTContext.
class Decl {
 public:
  virtual ~Decl() = default;

  Decl(const Decl&) = delete;
  Decl& operator=(const Decl&) = delete;

  DeclKind GetKind() const noexcept { return kind_; }
  SourceLocation GetLocation() const noexcept { return loc_; }

  template <typename T>
  const T* As() const noexcept {
    return T::ClassOf(this) ? static_cast<const T*>(this) : nullptr;
  }
  template <typename T>
  T* As() noexcept {
    return T::ClassOf(this) ? static_cast<T*>(this) : nullptr;
  }

 protected:
  Decl(DeclKind kind, SourceLocation loc) : kind_(kind), loc_(loc) {}

 private:
  DeclKind kind_;
  SourceLocation loc_;
};

/// \brief The root of the AST: the ordered list of file-scope declarations.
class TranslationUnitDecl final : public Decl {
 public:
  TranslationUnitDecl() : Decl(DeclKind::kTranslationUnit, {}) {}

  void AddDecl(Decl* d) { decls_.push_back(d); }
  const std::vector<Decl*>& GetDecls() const noexcept { return decls_; }

  static bool ClassOf(const Decl* d) noexcept {
    return d->GetKind() == DeclKind::kTranslationUnit;
  }

 private:
  std::vector<Decl*> decls_;
};

/// \brief A declaration with a name.
class NamedDecl : public Decl {
 public:
  const IdentifierInfo* GetIdentifier() const noexcept { return name_; }
  std::string_view GetName() const noexcept;

  static bool ClassOf(const Decl* d) noexcept {
    return d->GetKind() != DeclKind::kTranslationUnit &&
           d->GetKind() != DeclKind::kStaticAssert;
  }

 protected:
  NamedDecl(DeclKind kind, SourceLocation loc, const IdentifierInfo* name)
      : Decl(kind, loc), name_(name) {}

 private:
  const IdentifierInfo* name_;
};

/// \brief A named declaration that has a type (variables, functions, fields,
///        enum constants, typedefs).
class ValueDecl : public NamedDecl {
 public:
  QualType GetType() const noexcept { return type_; }
  void SetType(QualType t) noexcept { type_ = t; }

  static bool ClassOf(const Decl* d) noexcept {
    switch (d->GetKind()) {
      case DeclKind::kTypedef:
      case DeclKind::kVar:
      case DeclKind::kParmVar:
      case DeclKind::kFunction:
      case DeclKind::kField:
      case DeclKind::kEnumConstant:
        return true;
      default:
        return false;
    }
  }

 protected:
  ValueDecl(DeclKind kind, SourceLocation loc, const IdentifierInfo* name,
            QualType type)
      : NamedDecl(kind, loc, name), type_(type) {}

 private:
  QualType type_;
};

class TypedefDecl final : public ValueDecl {
 public:
  TypedefDecl(SourceLocation loc, const IdentifierInfo* name,
              QualType underlying)
      : ValueDecl(DeclKind::kTypedef, loc, name, underlying) {}

  static bool ClassOf(const Decl* d) noexcept {
    return d->GetKind() == DeclKind::kTypedef;
  }
};

class VarDecl : public ValueDecl {
 public:
  VarDecl(SourceLocation loc, const IdentifierInfo* name, QualType type,
          StorageClass sc, bool is_file_scope)
      : ValueDecl(DeclKind::kVar, loc, name, type),
        sc_(sc),
        is_file_scope_(is_file_scope) {}

  StorageClass GetStorageClass() const noexcept { return sc_; }
  bool IsFileScope() const noexcept { return is_file_scope_; }

  /// \brief True if this object has static storage duration (C11 6.2.4p3):
  ///        file scope, or block scope with static/extern.
  bool HasStaticStorage() const noexcept {
    return is_file_scope_ || sc_ == StorageClass::kStatic ||
           sc_ == StorageClass::kExtern;
  }

  const Expr* GetInit() const noexcept { return init_; }
  void SetInit(const Expr* init) noexcept { init_ = init; }
  bool HasInit() const noexcept { return init_ != nullptr; }

  static bool ClassOf(const Decl* d) noexcept {
    return d->GetKind() == DeclKind::kVar ||
           d->GetKind() == DeclKind::kParmVar;
  }

 protected:
  VarDecl(DeclKind kind, SourceLocation loc, const IdentifierInfo* name,
          QualType type, StorageClass sc)
      : ValueDecl(kind, loc, name, type), sc_(sc), is_file_scope_(false) {}

 private:
  const Expr* init_ = nullptr;
  StorageClass sc_;
  bool is_file_scope_;
};

/// \brief A function parameter. Its type is the *adjusted* type (arrays and
///        functions decayed to pointers, C11 6.7.6.3p7f).
class ParmVarDecl final : public VarDecl {
 public:
  ParmVarDecl(SourceLocation loc, const IdentifierInfo* name, QualType type)
      : VarDecl(DeclKind::kParmVar, loc, name, type, StorageClass::kNone) {}

  static bool ClassOf(const Decl* d) noexcept {
    return d->GetKind() == DeclKind::kParmVar;
  }
};

class FunctionDecl final : public ValueDecl {
 public:
  FunctionDecl(SourceLocation loc, const IdentifierInfo* name, QualType type,
               StorageClass sc, bool is_inline, bool is_noreturn)
      : ValueDecl(DeclKind::kFunction, loc, name, type),
        sc_(sc),
        is_inline_(is_inline),
        is_noreturn_(is_noreturn) {}

  StorageClass GetStorageClass() const noexcept { return sc_; }
  bool IsInline() const noexcept { return is_inline_; }
  bool IsNoreturn() const noexcept { return is_noreturn_; }

  const std::vector<ParmVarDecl*>& GetParams() const noexcept {
    return params_;
  }
  void SetParams(std::vector<ParmVarDecl*> params) {
    params_ = std::move(params);
  }

  QualType GetReturnType() const noexcept {
    return GetType()->AsCanonical<FunctionType>()->GetReturnType();
  }

  const CompoundStmt* GetBody() const noexcept { return body_; }
  void SetBody(const CompoundStmt* body) noexcept { body_ = body; }
  bool IsDefined() const noexcept { return body_ != nullptr; }

  static bool ClassOf(const Decl* d) noexcept {
    return d->GetKind() == DeclKind::kFunction;
  }

 private:
  std::vector<ParmVarDecl*> params_;
  const CompoundStmt* body_ = nullptr;
  StorageClass sc_;
  bool is_inline_;
  bool is_noreturn_;
};

/// \brief A struct/union member. An unnamed field whose type is an anonymous
///        struct/union is an *anonymous member*: its members are looked up as
///        if they belonged to the enclosing record (C11 6.7.2.1p13).
class FieldDecl final : public ValueDecl {
 public:
  FieldDecl(SourceLocation loc, const IdentifierInfo* name, QualType type,
            const Expr* bitwidth_expr, unsigned bitwidth)
      : ValueDecl(DeclKind::kField, loc, name, type),
        bitwidth_expr_(bitwidth_expr),
        bitwidth_(bitwidth) {}

  bool IsBitField() const noexcept { return bitwidth_expr_ != nullptr; }
  unsigned GetBitWidth() const noexcept { return bitwidth_; }
  const Expr* GetBitWidthExpr() const noexcept { return bitwidth_expr_; }

  static bool ClassOf(const Decl* d) noexcept {
    return d->GetKind() == DeclKind::kField;
  }

 private:
  const Expr* bitwidth_expr_;
  unsigned bitwidth_;
};

enum class TagKind : uint8_t { kStruct, kUnion, kEnum };

/// \brief Common base for struct/union/enum declarations: a tag that may be
///        forward-declared and later completed.
class TagDecl : public NamedDecl {
 public:
  TagKind GetTagKind() const noexcept { return tag_kind_; }
  bool IsCompleteDefinition() const noexcept { return is_complete_; }
  void SetCompleteDefinition() noexcept { is_complete_ = true; }

  static bool ClassOf(const Decl* d) noexcept {
    return d->GetKind() == DeclKind::kRecord ||
           d->GetKind() == DeclKind::kEnum;
  }

 protected:
  TagDecl(DeclKind kind, TagKind tag_kind, SourceLocation loc,
          const IdentifierInfo* name)
      : NamedDecl(kind, loc, name), tag_kind_(tag_kind) {}

 private:
  TagKind tag_kind_;
  bool is_complete_ = false;
};

class RecordDecl final : public TagDecl {
 public:
  RecordDecl(TagKind tag_kind, SourceLocation loc, const IdentifierInfo* name)
      : TagDecl(DeclKind::kRecord, tag_kind, loc, name) {}

  bool IsUnion() const noexcept { return GetTagKind() == TagKind::kUnion; }

  const std::vector<FieldDecl*>& GetFields() const noexcept { return fields_; }
  void AddField(FieldDecl* f) { fields_.push_back(f); }

  /// \brief Whether the last field is a flexible array member `T x[];`.
  bool HasFlexibleArrayMember() const noexcept { return has_flexible_array_; }
  void SetHasFlexibleArrayMember() noexcept { has_flexible_array_ = true; }

  /// \brief Finds a direct or anonymous-member field by name; returns null if
  ///        absent. \p path receives the chain of fields to traverse (the
  ///        found field last) so MemberExpr chains can be built.
  const FieldDecl* FindField(const IdentifierInfo* name,
                             std::vector<const FieldDecl*>* path
                             = nullptr) const;

  static bool ClassOf(const Decl* d) noexcept {
    return d->GetKind() == DeclKind::kRecord;
  }

 private:
  std::vector<FieldDecl*> fields_;
  bool has_flexible_array_ = false;
};

class EnumConstantDecl final : public ValueDecl {
 public:
  EnumConstantDecl(SourceLocation loc, const IdentifierInfo* name,
                   QualType type, int64_t value)
      : ValueDecl(DeclKind::kEnumConstant, loc, name, type), value_(value) {}

  int64_t GetValue() const noexcept { return value_; }

  static bool ClassOf(const Decl* d) noexcept {
    return d->GetKind() == DeclKind::kEnumConstant;
  }

 private:
  int64_t value_;
};

class EnumDecl final : public TagDecl {
 public:
  EnumDecl(SourceLocation loc, const IdentifierInfo* name)
      : TagDecl(DeclKind::kEnum, TagKind::kEnum, loc, name) {}

  const std::vector<EnumConstantDecl*>& GetEnumerators() const noexcept {
    return enumerators_;
  }
  void AddEnumerator(EnumConstantDecl* e) { enumerators_.push_back(e); }

  static bool ClassOf(const Decl* d) noexcept {
    return d->GetKind() == DeclKind::kEnum;
  }

 private:
  std::vector<EnumConstantDecl*> enumerators_;
};

/// \brief A goto label. Created on first reference or definition; defined once
///        its LabelStmt is seen.
class LabelDecl final : public NamedDecl {
 public:
  LabelDecl(SourceLocation loc, const IdentifierInfo* name)
      : NamedDecl(DeclKind::kLabel, loc, name) {}

  bool IsDefined() const noexcept { return is_defined_; }
  void SetDefined(SourceLocation loc) noexcept {
    is_defined_ = true;
    def_loc_ = loc;
  }
  SourceLocation GetDefinitionLoc() const noexcept { return def_loc_; }

  static bool ClassOf(const Decl* d) noexcept {
    return d->GetKind() == DeclKind::kLabel;
  }

 private:
  SourceLocation def_loc_;
  bool is_defined_ = false;
};

/// \brief A `_Static_assert(expr, "msg");` declaration, retained in the AST
///        after being checked.
class StaticAssertDecl final : public Decl {
 public:
  StaticAssertDecl(SourceLocation loc, const Expr* cond, std::string message)
      : Decl(DeclKind::kStaticAssert, loc),
        cond_(cond),
        message_(std::move(message)) {}

  const Expr* GetCond() const noexcept { return cond_; }
  std::string_view GetMessage() const noexcept { return message_; }

  static bool ClassOf(const Decl* d) noexcept {
    return d->GetKind() == DeclKind::kStaticAssert;
  }

 private:
  const Expr* cond_;
  std::string message_;
};

}  // namespace bcc
