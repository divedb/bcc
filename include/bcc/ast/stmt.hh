#pragma once

#include <cstdint>
#include <vector>

#include "bcc/basic/source_range.hh"

namespace bcc {

class Decl;
class Expr;
class LabelDecl;
class VarDecl;

enum class StmtClass : uint8_t {
  // Statements.
  kNullStmt,
  kCompoundStmt,
  kDeclStmt,
  kIfStmt,
  kWhileStmt,
  kDoStmt,
  kForStmt,
  kSwitchStmt,
  kCaseStmt,
  kDefaultStmt,
  kBreakStmt,
  kContinueStmt,
  kReturnStmt,
  kGotoStmt,
  kLabelStmt,
  // Expressions (Expr subclasses; kept in one enum like Clang's StmtClass).
  kIntegerLiteral,
  kFloatingLiteral,
  kCharacterLiteral,
  kStringLiteral,
  kDeclRefExpr,
  kParenExpr,
  kUnaryOperator,
  kBinaryOperator,
  kCompoundAssignOperator,
  kConditionalOperator,
  kArraySubscriptExpr,
  kCallExpr,
  kMemberExpr,
  kImplicitCastExpr,
  kCStyleCastExpr,
  kSizeOfAlignOfExpr,
  kCompoundLiteralExpr,
  kInitListExpr,
  kDesignatedInitExpr,
  kGenericSelectionExpr,
};

/// \brief Base of statements and (via Expr) expressions. Owned by ASTContext.
class Stmt {
 public:
  virtual ~Stmt() = default;

  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  StmtClass GetStmtClass() const noexcept { return sc_; }
  SourceRange GetSourceRange() const noexcept { return range_; }
  SourceLocation GetBeginLoc() const noexcept { return range_.begin; }
  SourceLocation GetEndLoc() const noexcept { return range_.end; }
  void SetSourceRange(SourceRange r) noexcept { range_ = r; }

  bool IsExpr() const noexcept { return sc_ >= StmtClass::kIntegerLiteral; }

  template <typename T>
  const T* As() const noexcept {
    return T::ClassOf(this) ? static_cast<const T*>(this) : nullptr;
  }
  template <typename T>
  T* As() noexcept {
    return T::ClassOf(this) ? static_cast<T*>(this) : nullptr;
  }

 protected:
  Stmt(StmtClass sc, SourceRange range) : sc_(sc), range_(range) {}

 private:
  StmtClass sc_;
  SourceRange range_;
};

/// The empty statement `;`.
class NullStmt final : public Stmt {
 public:
  explicit NullStmt(SourceLocation semi_loc)
      : Stmt(StmtClass::kNullStmt, {semi_loc, semi_loc}) {}

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kNullStmt;
  }
};

class CompoundStmt final : public Stmt {
 public:
  CompoundStmt(std::vector<Stmt*> body, SourceRange braces)
      : Stmt(StmtClass::kCompoundStmt, braces), body_(std::move(body)) {}

  const std::vector<Stmt*>& GetBody() const noexcept { return body_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kCompoundStmt;
  }

 private:
  std::vector<Stmt*> body_;
};

/// A declaration inside a block, e.g. `int x = 3, y;`.
class DeclStmt final : public Stmt {
 public:
  DeclStmt(std::vector<Decl*> decls, SourceRange range)
      : Stmt(StmtClass::kDeclStmt, range), decls_(std::move(decls)) {}

  const std::vector<Decl*>& GetDecls() const noexcept { return decls_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kDeclStmt;
  }

 private:
  std::vector<Decl*> decls_;
};

class IfStmt final : public Stmt {
 public:
  IfStmt(SourceLocation if_loc, const Expr* cond, Stmt* then_stmt,
         Stmt* else_stmt, SourceLocation end)
      : Stmt(StmtClass::kIfStmt, {if_loc, end}),
        cond_(cond),
        then_(then_stmt),
        else_(else_stmt) {}

  const Expr* GetCond() const noexcept { return cond_; }
  const Stmt* GetThen() const noexcept { return then_; }
  const Stmt* GetElse() const noexcept { return else_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kIfStmt;
  }

 private:
  const Expr* cond_;
  Stmt* then_;
  Stmt* else_;  // may be null
};

class WhileStmt final : public Stmt {
 public:
  WhileStmt(SourceLocation while_loc, const Expr* cond, Stmt* body,
            SourceLocation end)
      : Stmt(StmtClass::kWhileStmt, {while_loc, end}),
        cond_(cond),
        body_(body) {}

  const Expr* GetCond() const noexcept { return cond_; }
  const Stmt* GetBody() const noexcept { return body_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kWhileStmt;
  }

 private:
  const Expr* cond_;
  Stmt* body_;
};

class DoStmt final : public Stmt {
 public:
  DoStmt(SourceLocation do_loc, Stmt* body, const Expr* cond,
         SourceLocation end)
      : Stmt(StmtClass::kDoStmt, {do_loc, end}), cond_(cond), body_(body) {}

  const Expr* GetCond() const noexcept { return cond_; }
  const Stmt* GetBody() const noexcept { return body_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kDoStmt;
  }

 private:
  const Expr* cond_;
  Stmt* body_;
};

class ForStmt final : public Stmt {
 public:
  /// \param init Either a DeclStmt, an expression, or null.
  ForStmt(SourceLocation for_loc, Stmt* init, const Expr* cond,
          const Expr* inc, Stmt* body, SourceLocation end)
      : Stmt(StmtClass::kForStmt, {for_loc, end}),
        init_(init),
        cond_(cond),
        inc_(inc),
        body_(body) {}

  const Stmt* GetInit() const noexcept { return init_; }
  const Expr* GetCond() const noexcept { return cond_; }
  const Expr* GetInc() const noexcept { return inc_; }
  const Stmt* GetBody() const noexcept { return body_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kForStmt;
  }

 private:
  Stmt* init_;        // may be null
  const Expr* cond_;  // may be null
  const Expr* inc_;   // may be null
  Stmt* body_;
};

class CaseStmt;
class DefaultStmt;

class SwitchStmt final : public Stmt {
 public:
  SwitchStmt(SourceLocation switch_loc, const Expr* cond, Stmt* body,
             SourceLocation end)
      : Stmt(StmtClass::kSwitchStmt, {switch_loc, end}),
        cond_(cond),
        body_(body) {}

  const Expr* GetCond() const noexcept { return cond_; }
  const Stmt* GetBody() const noexcept { return body_; }

  /// The body is attached after parsing it; the SwitchStmt itself is created
  /// when the condition is seen so case labels can register against it.
  void SetBody(Stmt* body) noexcept {
    body_ = body;
    if (body) SetSourceRange({GetBeginLoc(), body->GetEndLoc()});
  }

  const std::vector<const CaseStmt*>& GetCases() const noexcept {
    return cases_;
  }
  void AddCase(const CaseStmt* c) { cases_.push_back(c); }
  const DefaultStmt* GetDefault() const noexcept { return default_; }
  void SetDefault(const DefaultStmt* d) noexcept { default_ = d; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kSwitchStmt;
  }

 private:
  const Expr* cond_;
  Stmt* body_;
  std::vector<const CaseStmt*> cases_;
  const DefaultStmt* default_ = nullptr;
};

class CaseStmt final : public Stmt {
 public:
  CaseStmt(SourceLocation case_loc, const Expr* value_expr, int64_t value,
           Stmt* sub)
      : Stmt(StmtClass::kCaseStmt, {case_loc, case_loc}),
        value_expr_(value_expr),
        value_(value),
        sub_(sub) {}

  const Expr* GetValueExpr() const noexcept { return value_expr_; }
  /// The case value converted to the promoted switch-condition type.
  int64_t GetValue() const noexcept { return value_; }
  const Stmt* GetSubStmt() const noexcept { return sub_; }
  void SetSubStmt(Stmt* s) noexcept { sub_ = s; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kCaseStmt;
  }

 private:
  const Expr* value_expr_;
  int64_t value_;
  Stmt* sub_;
};

class DefaultStmt final : public Stmt {
 public:
  DefaultStmt(SourceLocation default_loc, Stmt* sub)
      : Stmt(StmtClass::kDefaultStmt, {default_loc, default_loc}), sub_(sub) {}

  const Stmt* GetSubStmt() const noexcept { return sub_; }
  void SetSubStmt(Stmt* s) noexcept { sub_ = s; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kDefaultStmt;
  }

 private:
  Stmt* sub_;
};

class BreakStmt final : public Stmt {
 public:
  explicit BreakStmt(SourceLocation loc)
      : Stmt(StmtClass::kBreakStmt, {loc, loc}) {}

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kBreakStmt;
  }
};

class ContinueStmt final : public Stmt {
 public:
  explicit ContinueStmt(SourceLocation loc)
      : Stmt(StmtClass::kContinueStmt, {loc, loc}) {}

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kContinueStmt;
  }
};

class ReturnStmt final : public Stmt {
 public:
  ReturnStmt(SourceLocation return_loc, const Expr* value)
      : Stmt(StmtClass::kReturnStmt, {return_loc, return_loc}),
        value_(value) {}

  const Expr* GetValue() const noexcept { return value_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kReturnStmt;
  }

 private:
  const Expr* value_;  // may be null
};

class GotoStmt final : public Stmt {
 public:
  GotoStmt(SourceLocation goto_loc, const LabelDecl* label)
      : Stmt(StmtClass::kGotoStmt, {goto_loc, goto_loc}), label_(label) {}

  const LabelDecl* GetLabel() const noexcept { return label_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kGotoStmt;
  }

 private:
  const LabelDecl* label_;
};

class LabelStmt final : public Stmt {
 public:
  LabelStmt(SourceLocation label_loc, const LabelDecl* label, Stmt* sub)
      : Stmt(StmtClass::kLabelStmt, {label_loc, label_loc}),
        label_(label),
        sub_(sub) {}

  const LabelDecl* GetLabel() const noexcept { return label_; }
  const Stmt* GetSubStmt() const noexcept { return sub_; }

  static bool ClassOf(const Stmt* s) noexcept {
    return s->GetStmtClass() == StmtClass::kLabelStmt;
  }

 private:
  const LabelDecl* label_;
  Stmt* sub_;
};

}  // namespace bcc
