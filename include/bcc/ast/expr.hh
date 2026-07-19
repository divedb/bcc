#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "bcc/ast/stmt.hh"
#include "bcc/ast/type.hh"

namespace bcc {

class FieldDecl;
class IdentifierInfo;
class ValueDecl;

/// \brief Whether an expression designates an object (lvalue) or is a plain
///        value (rvalue). C has no xvalues.
enum class ValueKind : uint8_t { kRValue, kLValue };

/// \brief The kind of conversion an ImplicitCastExpr / CStyleCastExpr performs
///        (a subset of Clang's CastKind sufficient for C).
enum class CastKind : uint8_t {
  kLValueToRValue,
  kArrayToPointerDecay,
  kFunctionToPointerDecay,
  kIntegralCast,
  kIntegralToFloating,
  kFloatingToIntegral,
  kFloatingCast,
  kIntegralToBoolean,
  kFloatingToBoolean,
  kPointerToBoolean,
  kIntegralToPointer,
  kPointerToIntegral,
  kNullToPointer,
  kBitCast,  // pointer <-> pointer reinterpretation
  kToVoid,
  kNoOp,  // same canonical type; qualification-only change
};

std::string_view GetCastKindName(CastKind kind) noexcept;

/// \brief Base of all expressions. Every expression carries its type and value
///        kind, assigned by Sema; conversions appear explicitly as
///        ImplicitCastExpr nodes.
class Expr : public Stmt {
 public:
  QualType GetType() const noexcept { return type_; }
  void SetType(QualType t) noexcept { type_ = t; }

  ValueKind GetValueKind() const noexcept { return vk_; }
  bool IsLValue() const noexcept { return vk_ == ValueKind::kLValue; }

  /// \brief Strips ParenExpr wrappers.
  const Expr* IgnoreParens() const noexcept;

  /// \brief Strips ParenExpr and ImplicitCastExpr wrappers.
  const Expr* IgnoreParenImpCasts() const noexcept;

  /// \brief True if this is an integer literal 0 (after parens/implicit
  ///        casts), or such a literal cast to void* — a null pointer constant
  ///        (C11 6.3.2.3p3).
  bool IsNullPointerConstant() const noexcept;

  /// \brief True if this expression, after Sema, contains an error node.
  bool ContainsErrors() const noexcept { return contains_errors_; }
  void SetContainsErrors() noexcept { contains_errors_ = true; }

  /// \brief True for expressions a C compiler may modify through: an lvalue
  ///        that is not an array, not const-qualified, not an incomplete type,
  ///        and has no const-qualified member (C11 6.3.2.1p1).
  bool IsModifiableLValue() const noexcept;

  static bool ClassOf(const Stmt* s) noexcept { return s->IsExpr(); }

 protected:
  Expr(StmtClass sc, QualType type, ValueKind vk, SourceRange range)
      : Stmt(sc, range), type_(type), vk_(vk) {}

 private:
  QualType type_;
  ValueKind vk_;
  bool contains_errors_ = false;
};

class IntegerLiteral final : public Expr {
 public:
  IntegerLiteral(uint64_t value, QualType type, SourceLocation loc)
      : Expr(StmtClass::kIntegerLiteral, type, ValueKind::kRValue, {loc, loc}),
        value_(value) {}

  uint64_t GetValue() const noexcept { return value_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kIntegerLiteral;
  }

 private:
  uint64_t value_;
};

class FloatingLiteral final : public Expr {
 public:
  FloatingLiteral(double value, QualType type, SourceLocation loc)
      : Expr(StmtClass::kFloatingLiteral, type, ValueKind::kRValue,
             {loc, loc}),
        value_(value) {}

  double GetValue() const noexcept { return value_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kFloatingLiteral;
  }

 private:
  double value_;
};

/// A character constant. Type is int in C (C11 6.4.4.4p10).
class CharacterLiteral final : public Expr {
 public:
  CharacterLiteral(uint32_t value, QualType type, SourceLocation loc)
      : Expr(StmtClass::kCharacterLiteral, type, ValueKind::kRValue,
             {loc, loc}),
        value_(value) {}

  uint32_t GetValue() const noexcept { return value_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kCharacterLiteral;
  }

 private:
  uint32_t value_;
};

/// \brief A (possibly concatenated) string literal. Type is `char[N]` (or the
///        wide variant), N including the terminating NUL; it is an lvalue.
///        The stored bytes are the decoded element values, without the NUL.
class StringLiteral final : public Expr {
 public:
  StringLiteral(std::string bytes, unsigned char_byte_width, QualType type,
                SourceLocation loc)
      : Expr(StmtClass::kStringLiteral, type, ValueKind::kLValue, {loc, loc}),
        bytes_(std::move(bytes)),
        char_byte_width_(char_byte_width) {}

  /// The decoded element bytes (little-endian for wide elements), sans NUL.
  std::string_view GetBytes() const noexcept { return bytes_; }
  unsigned GetCharByteWidth() const noexcept { return char_byte_width_; }
  /// Number of elements, not counting the terminating NUL.
  uint64_t GetLength() const noexcept {
    return bytes_.size() / char_byte_width_;
  }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kStringLiteral;
  }

 private:
  std::string bytes_;
  unsigned char_byte_width_;
};

/// A use of a declared variable, function, or enum constant.
class DeclRefExpr final : public Expr {
 public:
  DeclRefExpr(const ValueDecl* decl, QualType type, ValueKind vk,
              SourceLocation loc)
      : Expr(StmtClass::kDeclRefExpr, type, vk, {loc, loc}), decl_(decl) {}

  const ValueDecl* GetDecl() const noexcept { return decl_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kDeclRefExpr;
  }

 private:
  const ValueDecl* decl_;
};

class ParenExpr final : public Expr {
 public:
  ParenExpr(const Expr* sub, SourceLocation lparen, SourceLocation rparen)
      : Expr(StmtClass::kParenExpr, sub->GetType(), sub->GetValueKind(),
             {lparen, rparen}),
        sub_(sub) {}

  const Expr* GetSubExpr() const noexcept { return sub_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kParenExpr;
  }

 private:
  const Expr* sub_;
};

enum class UnaryOperatorKind : uint8_t {
  kPostInc,
  kPostDec,
  kPreInc,
  kPreDec,
  kAddrOf,   // &
  kDeref,    // *
  kPlus,     // +
  kMinus,    // -
  kNot,      // ~
  kLNot,     // !
};

std::string_view GetUnaryOperatorSpelling(UnaryOperatorKind op) noexcept;

class UnaryOperator final : public Expr {
 public:
  UnaryOperator(UnaryOperatorKind op, const Expr* sub, QualType type,
                ValueKind vk, SourceLocation op_loc)
      : Expr(StmtClass::kUnaryOperator, type, vk,
             op == UnaryOperatorKind::kPostInc ||
                     op == UnaryOperatorKind::kPostDec
                 ? SourceRange{sub->GetBeginLoc(), op_loc}
                 : SourceRange{op_loc, sub->GetEndLoc()}),
        sub_(sub),
        op_(op),
        op_loc_(op_loc) {}

  UnaryOperatorKind GetOpcode() const noexcept { return op_; }
  const Expr* GetSubExpr() const noexcept { return sub_; }
  SourceLocation GetOperatorLoc() const noexcept { return op_loc_; }

  bool IsIncrementDecrementOp() const noexcept {
    return op_ <= UnaryOperatorKind::kPreDec;
  }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kUnaryOperator;
  }

 private:
  const Expr* sub_;
  UnaryOperatorKind op_;
  SourceLocation op_loc_;
};

enum class BinaryOperatorKind : uint8_t {
  kMul,
  kDiv,
  kRem,
  kAdd,
  kSub,
  kShl,
  kShr,
  kLT,
  kGT,
  kLE,
  kGE,
  kEQ,
  kNE,
  kAnd,   // &
  kXor,   // ^
  kOr,    // |
  kLAnd,  // &&
  kLOr,   // ||
  kAssign,
  kMulAssign,
  kDivAssign,
  kRemAssign,
  kAddAssign,
  kSubAssign,
  kShlAssign,
  kShrAssign,
  kAndAssign,
  kXorAssign,
  kOrAssign,
  kComma,
};

std::string_view GetBinaryOperatorSpelling(BinaryOperatorKind op) noexcept;

class BinaryOperator : public Expr {
 public:
  BinaryOperator(BinaryOperatorKind op, const Expr* lhs, const Expr* rhs,
                 QualType type, ValueKind vk, SourceLocation op_loc)
      : BinaryOperator(StmtClass::kBinaryOperator, op, lhs, rhs, type, vk,
                       op_loc) {}

  BinaryOperatorKind GetOpcode() const noexcept { return op_; }
  const Expr* GetLHS() const noexcept { return lhs_; }
  const Expr* GetRHS() const noexcept { return rhs_; }
  SourceLocation GetOperatorLoc() const noexcept { return op_loc_; }

  bool IsAssignmentOp() const noexcept {
    return op_ >= BinaryOperatorKind::kAssign &&
           op_ <= BinaryOperatorKind::kOrAssign;
  }
  bool IsComparisonOp() const noexcept {
    return op_ >= BinaryOperatorKind::kLT && op_ <= BinaryOperatorKind::kNE;
  }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kBinaryOperator ||
           s->GetStmtClass() == StmtClass::kCompoundAssignOperator;
  }

 protected:
  BinaryOperator(StmtClass sc, BinaryOperatorKind op, const Expr* lhs,
                 const Expr* rhs, QualType type, ValueKind vk,
                 SourceLocation op_loc)
      : Expr(sc, type, vk, {lhs->GetBeginLoc(), rhs->GetEndLoc()}),
        lhs_(lhs),
        rhs_(rhs),
        op_(op),
        op_loc_(op_loc) {}

 private:
  const Expr* lhs_;
  const Expr* rhs_;
  BinaryOperatorKind op_;
  SourceLocation op_loc_;
};

/// \brief `a += b` etc. Also records the type the LHS is promoted to for the
///        computation and the type of the result before conversion back,
///        mirroring Clang's CompoundAssignOperator.
class CompoundAssignOperator final : public BinaryOperator {
 public:
  CompoundAssignOperator(BinaryOperatorKind op, const Expr* lhs,
                         const Expr* rhs, QualType type,
                         QualType computation_type, SourceLocation op_loc)
      : BinaryOperator(StmtClass::kCompoundAssignOperator, op, lhs, rhs, type,
                       ValueKind::kRValue, op_loc),
        computation_type_(computation_type) {}

  QualType GetComputationType() const noexcept { return computation_type_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kCompoundAssignOperator;
  }

 private:
  QualType computation_type_;
};

class ConditionalOperator final : public Expr {
 public:
  ConditionalOperator(const Expr* cond, const Expr* lhs, const Expr* rhs,
                      QualType type)
      : Expr(StmtClass::kConditionalOperator, type, ValueKind::kRValue,
             {cond->GetBeginLoc(), rhs->GetEndLoc()}),
        cond_(cond),
        lhs_(lhs),
        rhs_(rhs) {}

  const Expr* GetCond() const noexcept { return cond_; }
  const Expr* GetTrueExpr() const noexcept { return lhs_; }
  const Expr* GetFalseExpr() const noexcept { return rhs_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kConditionalOperator;
  }

 private:
  const Expr* cond_;
  const Expr* lhs_;
  const Expr* rhs_;
};

class ArraySubscriptExpr final : public Expr {
 public:
  ArraySubscriptExpr(const Expr* base, const Expr* idx, QualType type,
                     SourceLocation rsquare)
      : Expr(StmtClass::kArraySubscriptExpr, type, ValueKind::kLValue,
             {base->GetBeginLoc(), rsquare}),
        base_(base),
        idx_(idx) {}

  /// The pointer operand (after any decay), regardless of written order.
  const Expr* GetBase() const noexcept { return base_; }
  const Expr* GetIdx() const noexcept { return idx_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kArraySubscriptExpr;
  }

 private:
  const Expr* base_;
  const Expr* idx_;
};

class CallExpr final : public Expr {
 public:
  CallExpr(const Expr* callee, std::vector<const Expr*> args, QualType type,
           SourceLocation rparen)
      : Expr(StmtClass::kCallExpr, type, ValueKind::kRValue,
             {callee->GetBeginLoc(), rparen}),
        callee_(callee),
        args_(std::move(args)) {}

  const Expr* GetCallee() const noexcept { return callee_; }
  const std::vector<const Expr*>& GetArgs() const noexcept { return args_; }
  unsigned GetNumArgs() const noexcept {
    return static_cast<unsigned>(args_.size());
  }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kCallExpr;
  }

 private:
  const Expr* callee_;
  std::vector<const Expr*> args_;
};

/// `base.field` or `base->field`.
class MemberExpr final : public Expr {
 public:
  MemberExpr(const Expr* base, bool is_arrow, const FieldDecl* member,
             QualType type, ValueKind vk, SourceLocation member_loc)
      : Expr(StmtClass::kMemberExpr, type, vk,
             {base->GetBeginLoc(), member_loc}),
        base_(base),
        member_(member),
        is_arrow_(is_arrow) {}

  const Expr* GetBase() const noexcept { return base_; }
  const FieldDecl* GetMember() const noexcept { return member_; }
  bool IsArrow() const noexcept { return is_arrow_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kMemberExpr;
  }

 private:
  const Expr* base_;
  const FieldDecl* member_;
  bool is_arrow_;
};

class CastExpr : public Expr {
 public:
  CastKind GetCastKind() const noexcept { return kind_; }
  const Expr* GetSubExpr() const noexcept { return sub_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kImplicitCastExpr ||
           s->GetStmtClass() == StmtClass::kCStyleCastExpr;
  }

 protected:
  CastExpr(StmtClass sc, CastKind kind, const Expr* sub, QualType type,
           ValueKind vk, SourceRange range)
      : Expr(sc, type, vk, range), sub_(sub), kind_(kind) {}

 private:
  const Expr* sub_;
  CastKind kind_;
};

/// A conversion materialized by Sema; has no source spelling of its own.
class ImplicitCastExpr final : public CastExpr {
 public:
  ImplicitCastExpr(CastKind kind, const Expr* sub, QualType type)
      : CastExpr(StmtClass::kImplicitCastExpr, kind, sub, type,
                 ValueKind::kRValue, sub->GetSourceRange()) {}

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kImplicitCastExpr;
  }
};

/// `(T)expr`.
class CStyleCastExpr final : public CastExpr {
 public:
  CStyleCastExpr(CastKind kind, const Expr* sub, QualType type,
                 SourceLocation lparen)
      : CastExpr(StmtClass::kCStyleCastExpr, kind, sub, type,
                 ValueKind::kRValue, {lparen, sub->GetEndLoc()}) {}

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kCStyleCastExpr;
  }
};

/// `sizeof expr`, `sizeof(T)`, `_Alignof(T)`. Value is computed by Sema at
/// creation (no VLAs), so this node is always an integer constant.
class SizeOfAlignOfExpr final : public Expr {
 public:
  SizeOfAlignOfExpr(bool is_sizeof, QualType operand_type, const Expr* operand,
                    uint64_t value, QualType type, SourceRange range)
      : Expr(StmtClass::kSizeOfAlignOfExpr, type, ValueKind::kRValue, range),
        operand_type_(operand_type),
        operand_(operand),
        value_(value),
        is_sizeof_(is_sizeof) {}

  bool IsSizeOf() const noexcept { return is_sizeof_; }
  bool IsArgumentType() const noexcept { return operand_ == nullptr; }
  QualType GetArgumentType() const noexcept { return operand_type_; }
  const Expr* GetArgumentExpr() const noexcept { return operand_; }
  uint64_t GetValue() const noexcept { return value_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kSizeOfAlignOfExpr;
  }

 private:
  QualType operand_type_;  // the queried type (operand's type for exprs)
  const Expr* operand_;    // null for sizeof(T)/_Alignof(T)
  uint64_t value_;
  bool is_sizeof_;
};

/// `(T){ init-list }` (C11 6.5.2.5).
class CompoundLiteralExpr final : public Expr {
 public:
  CompoundLiteralExpr(QualType type, const Expr* init, bool is_file_scope,
                      SourceLocation lparen)
      : Expr(StmtClass::kCompoundLiteralExpr, type, ValueKind::kLValue,
             {lparen, init->GetEndLoc()}),
        init_(init),
        is_file_scope_(is_file_scope) {}

  const Expr* GetInitializer() const noexcept { return init_; }
  bool IsFileScope() const noexcept { return is_file_scope_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kCompoundLiteralExpr;
  }

 private:
  const Expr* init_;
  bool is_file_scope_;
};

/// \brief A brace-enclosed initializer list.
///
/// Before semantic analysis (the "syntactic" form built by the parser) the
/// inits are the written expressions in order and type is null. Sema's
/// initialization checking produces a "semantic" form: one init per element /
/// field of the initialized aggregate, in layout order, with nulls for
/// implicitly zero-initialized sub-objects.
class InitListExpr final : public Expr {
 public:
  InitListExpr(std::vector<const Expr*> inits, SourceRange braces)
      : Expr(StmtClass::kInitListExpr, QualType(), ValueKind::kRValue, braces),
        inits_(std::move(inits)) {}

  const std::vector<const Expr*>& GetInits() const noexcept { return inits_; }
  void SetInit(unsigned i, const Expr* e) { inits_[i] = e; }

  /// For a semantic-form union initializer: the field that is initialized.
  const FieldDecl* GetInitializedField() const noexcept { return field_; }
  void SetInitializedField(const FieldDecl* f) noexcept { field_ = f; }

  bool IsSemanticForm() const noexcept { return !GetType().IsNull(); }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kInitListExpr;
  }

 private:
  std::vector<const Expr*> inits_;
  const FieldDecl* field_ = nullptr;
};

/// \brief One designator in a designated initializer: `.field` or `[index]`.
struct Designator {
  const IdentifierInfo* field = nullptr;  ///< set for `.field`
  const Expr* index = nullptr;            ///< set for `[index]`
  SourceLocation loc;

  bool IsField() const noexcept { return field != nullptr; }
};

/// \brief `.a[2].b = init` inside an initializer list (C11 6.7.9p6). Appears
///        only in the syntactic form of an InitListExpr; the semantic form
///        resolves designators away.
class DesignatedInitExpr final : public Expr {
 public:
  DesignatedInitExpr(std::vector<Designator> designators, const Expr* init)
      : Expr(StmtClass::kDesignatedInitExpr, QualType(), ValueKind::kRValue,
             init->GetSourceRange()),
        designators_(std::move(designators)),
        init_(init) {}

  const std::vector<Designator>& GetDesignators() const noexcept {
    return designators_;
  }
  const Expr* GetInit() const noexcept { return init_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kDesignatedInitExpr;
  }

 private:
  std::vector<Designator> designators_;
  const Expr* init_;
};

/// `_Generic(expr, T1: e1, default: e2)` — resolved by Sema; the AST keeps
/// only the chosen expression plus the controlling operand for fidelity.
class GenericSelectionExpr final : public Expr {
 public:
  GenericSelectionExpr(const Expr* controlling, const Expr* chosen,
                       SourceRange range)
      : Expr(StmtClass::kGenericSelectionExpr, chosen->GetType(),
             chosen->GetValueKind(), range),
        controlling_(controlling),
        chosen_(chosen) {}

  const Expr* GetControllingExpr() const noexcept { return controlling_; }
  const Expr* GetChosenExpr() const noexcept { return chosen_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kGenericSelectionExpr;
  }

 private:
  const Expr* controlling_;
  const Expr* chosen_;
};

}  // namespace bcc
