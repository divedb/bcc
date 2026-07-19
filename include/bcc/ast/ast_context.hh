#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "bcc/ast/decl.hh"
#include "bcc/ast/type.hh"

namespace bcc {

class Stmt;

/// \brief Layout of one struct/union: total size/alignment and per-field bit
///        offsets, computed lazily and cached by the ASTContext.
struct RecordLayout {
  uint64_t size = 0;   ///< sizeof, in bytes (includes tail padding).
  uint64_t align = 1;  ///< alignof, in bytes.
  std::vector<uint64_t> field_offsets_bits;  ///< one per field, in bits.
};

/// \brief Owns every AST node and uniques all structural types, so canonical
///        type equality is pointer equality. Also the target-layout oracle
///        (x86-64 SysV) answering sizeof/alignof queries.
class ASTContext {
 public:
  ASTContext();

  ASTContext(const ASTContext&) = delete;
  ASTContext& operator=(const ASTContext&) = delete;

  ~ASTContext();

  TranslationUnitDecl* GetTranslationUnit() noexcept { return tu_; }
  const TranslationUnitDecl* GetTranslationUnit() const noexcept {
    return tu_;
  }

  //===--------------------------------------------------------------------===//
  // Node creation. All nodes live for the lifetime of the ASTContext.
  //===--------------------------------------------------------------------===//

  template <typename T, typename... Args>
  T* New(Args&&... args) {
    auto owned = std::make_unique<T>(std::forward<Args>(args)...);
    T* raw = owned.get();
    if constexpr (std::is_base_of_v<Stmt, T>) {
      stmts_.push_back(std::move(owned));
    } else {
      static_assert(std::is_base_of_v<Decl, T>);
      decls_.push_back(std::move(owned));
    }
    return raw;
  }

  //===--------------------------------------------------------------------===//
  // Builtin types.
  //===--------------------------------------------------------------------===//

  QualType VoidTy() const noexcept { return builtins_[0]; }
  QualType BoolTy() const noexcept { return builtins_[1]; }
  QualType CharTy() const noexcept { return builtins_[2]; }
  QualType SCharTy() const noexcept { return builtins_[3]; }
  QualType UCharTy() const noexcept { return builtins_[4]; }
  QualType ShortTy() const noexcept { return builtins_[5]; }
  QualType UShortTy() const noexcept { return builtins_[6]; }
  QualType IntTy() const noexcept { return builtins_[7]; }
  QualType UIntTy() const noexcept { return builtins_[8]; }
  QualType LongTy() const noexcept { return builtins_[9]; }
  QualType ULongTy() const noexcept { return builtins_[10]; }
  QualType LongLongTy() const noexcept { return builtins_[11]; }
  QualType ULongLongTy() const noexcept { return builtins_[12]; }
  QualType FloatTy() const noexcept { return builtins_[13]; }
  QualType DoubleTy() const noexcept { return builtins_[14]; }
  QualType LongDoubleTy() const noexcept { return builtins_[15]; }

  QualType GetBuiltinType(BuiltinTypeKind kind) const noexcept {
    return builtins_[static_cast<unsigned>(kind)];
  }

  /// The type of `sizeof` results: unsigned long on x86-64 SysV.
  QualType SizeTy() const noexcept { return ULongTy(); }
  /// The type of pointer subtraction results: long on x86-64 SysV.
  QualType PtrdiffTy() const noexcept { return LongTy(); }
  /// wchar_t under the hood: int on x86-64 SysV.
  QualType WCharTy() const noexcept { return IntTy(); }
  QualType Char16Ty() const noexcept { return UShortTy(); }
  QualType Char32Ty() const noexcept { return UIntTy(); }

  //===--------------------------------------------------------------------===//
  // Type construction (uniqued).
  //===--------------------------------------------------------------------===//

  QualType GetPointerType(QualType pointee);
  QualType GetConstantArrayType(QualType element, uint64_t size);
  QualType GetIncompleteArrayType(QualType element);
  QualType GetVariableArrayType(QualType element, const Expr* size_expr);
  QualType GetFunctionType(QualType ret, std::vector<QualType> params,
                           bool is_variadic);
  QualType GetFunctionNoProtoType(QualType ret);
  /// Per-decl (not uniqued beyond the decl): the RecordType/EnumType for a
  /// tag, created on first request and cached on this context.
  QualType GetTagType(const TagDecl* tag);
  QualType GetTypedefType(const TypedefDecl* decl);

  /// \brief `char[len+1]` etc. for a string literal of \p len elements.
  QualType GetStringLiteralArrayType(QualType element, uint64_t len);

  //===--------------------------------------------------------------------===//
  // Type queries.
  //===--------------------------------------------------------------------===//

  /// \brief C11 6.2.7 type compatibility, on any (possibly sugared) types.
  bool IsCompatible(QualType a, QualType b) const;

  /// \brief The composite type (C11 6.2.7p3) of two compatible types.
  ///        Returns a null QualType if the types are not compatible.
  QualType GetCompositeType(QualType a, QualType b);

  /// \brief C11 6.3.2.1: array-of-T decays to pointer-to-T, function to
  ///        pointer-to-function. Returns \p t unchanged otherwise.
  QualType GetDecayedType(QualType t);

  /// \brief The integer conversion rank order (C11 6.3.1.1p1). Larger is
  ///        higher rank. Enum types rank as their underlying int.
  int GetIntegerRank(QualType t) const;

  /// \brief The unsigned integer type corresponding to \p t (which must be a
  ///        signed integer type).
  QualType GetCorrespondingUnsignedType(QualType t) const;

  /// \brief Whether \p t is a signed integer (or enum) type.
  bool IsSignedIntegerType(QualType t) const;
  bool IsUnsignedIntegerType(QualType t) const;

  /// \brief The promoted type of \p t under the integer promotions
  ///        (C11 6.3.1.1p2); returns \p t if unaffected.
  QualType GetPromotedIntegerType(QualType t) const;

  //===--------------------------------------------------------------------===//
  // Layout (x86-64 SysV).
  //===--------------------------------------------------------------------===//

  /// \brief sizeof(\p t) in bytes. \p t must be complete and not a VLA.
  uint64_t GetTypeSize(QualType t) const;

  /// \brief _Alignof(\p t) in bytes. \p t must be complete.
  uint64_t GetTypeAlign(QualType t) const;

  /// \brief Width in bits of the integer type \p t (e.g. 32 for int; 1 for
  ///        _Bool's value representation is still 8 stored bits — this
  ///        returns the *stored* width, 8 for _Bool).
  uint64_t GetIntWidth(QualType t) const { return GetTypeSize(t) * 8; }

  const RecordLayout& GetRecordLayout(const RecordDecl* record) const;

 private:
  void InitBuiltinTypes();
  const Type* Own(std::unique_ptr<Type> t, QualType canonical);
  static bool IsCompatibleCanonical(QualType a, QualType b);

  TranslationUnitDecl* tu_ = nullptr;

  std::vector<std::unique_ptr<Type>> types_;
  std::vector<std::unique_ptr<Decl>> decls_;
  std::vector<std::unique_ptr<Stmt>> stmts_;

  QualType builtins_[16];

  std::map<QualType, const Type*> pointer_types_;
  std::map<std::pair<QualType, uint64_t>, const Type*> constant_array_types_;
  std::map<QualType, const Type*> incomplete_array_types_;

  struct FunctionTypeKey {
    QualType ret;
    std::vector<QualType> params;
    bool variadic;
    bool no_proto;

    bool operator<(const FunctionTypeKey& o) const noexcept {
      return std::tie(ret, params, variadic, no_proto) <
             std::tie(o.ret, o.params, o.variadic, o.no_proto);
    }
  };
  std::map<FunctionTypeKey, const Type*> function_types_;

  std::map<const TagDecl*, const Type*> tag_types_;
  std::map<const TypedefDecl*, const Type*> typedef_types_;

  mutable std::map<const RecordDecl*, RecordLayout> record_layouts_;
};

}  // namespace bcc
