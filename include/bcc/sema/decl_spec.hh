#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "bcc/ast/type.hh"
#include "bcc/basic/source_location.hh"

namespace bcc {

class Expr;
class IdentifierInfo;
class ParmVarDecl;
class TagDecl;
class TypedefDecl;

/// \brief Accumulates the declaration specifiers of one declaration as the
///        parser reads them, with enough source information to diagnose
///        invalid combinations (mirrors Clang's DeclSpec).
class DeclSpec {
 public:
  enum class SCS : uint8_t {
    kUnspecified,
    kTypedef,
    kExtern,
    kStatic,
    kAuto,
    kRegister,
  };

  enum class TSW : uint8_t { kUnspecified, kShort, kLong, kLongLong };
  enum class TSS : uint8_t { kUnspecified, kSigned, kUnsigned };

  enum class TST : uint8_t {
    kUnspecified,
    kVoid,
    kChar,
    kInt,
    kFloat,
    kDouble,
    kBool,
    kStruct,
    kUnion,
    kEnum,
    kTypedefName,
  };

  //===--------------------------------------------------------------------===//
  // Setters. Each returns true on success; on conflict it returns false and
  // sets \p prev_spec to the spelling of the conflicting prior specifier.
  //===--------------------------------------------------------------------===//

  bool SetStorageClass(SCS sc, SourceLocation loc, std::string_view& prev_spec);
  bool SetTypeSpecWidth(TSW w, SourceLocation loc, std::string_view& prev_spec);
  bool SetTypeSpecSign(TSS s, SourceLocation loc, std::string_view& prev_spec);
  bool SetTypeSpecType(TST t, SourceLocation loc, std::string_view& prev_spec);

  /// \brief Records a type qualifier; duplicate qualifiers are legal in C11.
  void SetTypeQual(uint8_t qual_bit, SourceLocation loc);

  void SetInline(SourceLocation loc) { is_inline_ = true; NoteLoc(loc); }
  void SetNoreturn(SourceLocation loc) { is_noreturn_ = true; NoteLoc(loc); }

  /// For TST kStruct/kUnion/kEnum: the tag declaration.
  void SetTagDecl(TagDecl* tag) { tag_decl_ = tag; }
  /// For TST kTypedefName: the typedef being referenced.
  void SetTypedefDecl(TypedefDecl* td) { typedef_decl_ = td; }

  //===--------------------------------------------------------------------===//
  // Accessors.
  //===--------------------------------------------------------------------===//

  SCS GetStorageClass() const noexcept { return scs_; }
  TSW GetTypeSpecWidth() const noexcept { return tsw_; }
  TSS GetTypeSpecSign() const noexcept { return tss_; }
  TST GetTypeSpecType() const noexcept { return tst_; }
  uint8_t GetTypeQuals() const noexcept { return type_quals_; }
  bool IsInline() const noexcept { return is_inline_; }
  bool IsNoreturn() const noexcept { return is_noreturn_; }
  TagDecl* GetTagDecl() const noexcept { return tag_decl_; }
  TypedefDecl* GetTypedefDecl() const noexcept { return typedef_decl_; }

  /// \brief Whether any specifier at all has been written.
  bool IsEmpty() const noexcept {
    return scs_ == SCS::kUnspecified && !HasTypeSpecifier() &&
           type_quals_ == 0 && !is_inline_ && !is_noreturn_;
  }

  /// \brief Whether any *type* specifier (width/sign/type) has been written.
  bool HasTypeSpecifier() const noexcept {
    return tsw_ != TSW::kUnspecified || tss_ != TSS::kUnspecified ||
           tst_ != TST::kUnspecified;
  }

  SourceLocation GetBeginLoc() const noexcept { return begin_loc_; }
  SourceLocation GetStorageClassLoc() const noexcept { return scs_loc_; }
  SourceLocation GetTypeSpecLoc() const noexcept { return tst_loc_; }

  static std::string_view GetSpecifierName(SCS sc) noexcept;
  static std::string_view GetSpecifierName(TSW w) noexcept;
  static std::string_view GetSpecifierName(TSS s) noexcept;
  static std::string_view GetSpecifierName(TST t) noexcept;

 private:
  void NoteLoc(SourceLocation loc) {
    if (!begin_loc_.IsValid()) begin_loc_ = loc;
  }

  SCS scs_ = SCS::kUnspecified;
  TSW tsw_ = TSW::kUnspecified;
  TSS tss_ = TSS::kUnspecified;
  TST tst_ = TST::kUnspecified;
  uint8_t type_quals_ = 0;  // Qualifiers:: bits
  bool is_inline_ = false;
  bool is_noreturn_ = false;

  TagDecl* tag_decl_ = nullptr;
  TypedefDecl* typedef_decl_ = nullptr;

  SourceLocation begin_loc_;
  SourceLocation scs_loc_;
  SourceLocation tst_loc_;
};

/// \brief The syntactic context a declarator appears in; controls which
///        forms are legal (e.g. abstract declarators, storage classes).
enum class DeclaratorContext : uint8_t {
  kFile,       ///< File-scope declaration or function definition.
  kBlock,      ///< Declaration inside a compound statement.
  kForInit,    ///< Declaration in a for-statement init clause.
  kPrototype,  ///< Function parameter.
  kMember,     ///< struct/union member.
  kTypeName,   ///< Type-name (cast, sizeof) — abstract declarator only.
};

/// \brief One level of declarator structure: a pointer, array, or function
///        layer wrapped around the declared entity (Clang's DeclaratorChunk).
struct DeclaratorChunk {
  enum class Kind : uint8_t { kPointer, kArray, kFunction };

  /// Parameter of a function chunk. Sema fills \p decl when the parameter is
  /// declared in a prototype scope.
  struct ParamInfo {
    const IdentifierInfo* name = nullptr;
    SourceLocation loc;
    ParmVarDecl* decl = nullptr;
  };

  Kind kind;
  SourceLocation loc;

  // kPointer
  uint8_t pointer_quals = 0;

  // kArray
  Expr* array_size = nullptr;  ///< null for `[]` and `[*]`
  bool array_has_static = false;
  bool array_is_star = false;
  uint8_t array_quals = 0;

  // kFunction
  bool fun_has_proto = false;  ///< false for `()`
  bool fun_is_variadic = false;
  std::vector<ParamInfo> params;

  static DeclaratorChunk MakePointer(uint8_t quals, SourceLocation loc) {
    DeclaratorChunk c{Kind::kPointer, loc};
    c.pointer_quals = quals;
    return c;
  }

  static DeclaratorChunk MakeArray(Expr* size, bool has_static, bool is_star,
                                   uint8_t quals, SourceLocation loc) {
    DeclaratorChunk c{Kind::kArray, loc};
    c.array_size = size;
    c.array_has_static = has_static;
    c.array_is_star = is_star;
    c.array_quals = quals;
    return c;
  }

  static DeclaratorChunk MakeFunction(bool has_proto, bool is_variadic,
                                      std::vector<ParamInfo> params,
                                      SourceLocation loc) {
    DeclaratorChunk c{Kind::kFunction, loc};
    c.fun_has_proto = has_proto;
    c.fun_is_variadic = is_variadic;
    c.params = std::move(params);
    return c;
  }
};

/// \brief A parsed declarator: the DeclSpec it modifies, the declared
///        identifier (if any), and the chunk list ordered from the identifier
///        outward — chunks_[0] binds tightest (mirrors Clang's Declarator).
class Declarator {
 public:
  Declarator(const DeclSpec& ds, DeclaratorContext context)
      : decl_spec_(ds), context_(context) {}

  const DeclSpec& GetDeclSpec() const noexcept { return decl_spec_; }
  DeclaratorContext GetContext() const noexcept { return context_; }

  void SetIdentifier(const IdentifierInfo* id, SourceLocation loc) noexcept {
    identifier_ = id;
    identifier_loc_ = loc;
  }
  const IdentifierInfo* GetIdentifier() const noexcept { return identifier_; }
  SourceLocation GetIdentifierLoc() const noexcept { return identifier_loc_; }
  bool HasName() const noexcept { return identifier_ != nullptr; }

  void AddChunk(DeclaratorChunk chunk) { chunks_.push_back(std::move(chunk)); }
  const std::vector<DeclaratorChunk>& GetChunks() const noexcept {
    return chunks_;
  }
  std::vector<DeclaratorChunk>& GetChunks() noexcept { return chunks_; }

  /// \brief The innermost function chunk is what makes `f(int)` a function
  ///        declarator; used to decide function-definition parsing.
  bool IsFunctionDeclarator() const noexcept {
    return !chunks_.empty() &&
           chunks_.front().kind == DeclaratorChunk::Kind::kFunction;
  }

  void SetInvalid() noexcept { invalid_ = true; }
  bool IsInvalid() const noexcept { return invalid_; }

 private:
  const DeclSpec& decl_spec_;
  DeclaratorContext context_;
  const IdentifierInfo* identifier_ = nullptr;
  SourceLocation identifier_loc_;
  std::vector<DeclaratorChunk> chunks_;
  bool invalid_ = false;
};

}  // namespace bcc
