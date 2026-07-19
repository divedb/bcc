#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "bcc/ast/ast_context.hh"
#include "bcc/ast/decl.hh"
#include "bcc/ast/expr.hh"
#include "bcc/ast/stmt.hh"
#include "bcc/basic/diagnostic.hh"
#include "bcc/sema/decl_spec.hh"
#include "bcc/sema/scope.hh"

namespace bcc {

class Preprocessor;
class Token;

/// \brief A value-or-invalid result flowing from Sema back to the parser
///        (mirrors Clang's ActionResult). A null value with invalid_ unset is
///        "no value but fine" (e.g. an empty optional expression).
template <typename T>
class ActionResult {
 public:
  ActionResult() : val_(nullptr), invalid_(false) {}
  ActionResult(T val) : val_(val), invalid_(false) {}

  static ActionResult MakeInvalid() {
    ActionResult r;
    r.invalid_ = true;
    return r;
  }

  bool IsInvalid() const noexcept { return invalid_; }
  bool IsUsable() const noexcept { return !invalid_ && val_ != nullptr; }
  T Get() const noexcept { return val_; }

 private:
  T val_;
  bool invalid_;
};

using ExprResult = ActionResult<Expr*>;
using StmtResult = ActionResult<Stmt*>;
using DeclResult = ActionResult<Decl*>;

inline ExprResult ExprError() { return ExprResult::MakeInvalid(); }
inline StmtResult StmtError() { return StmtResult::MakeInvalid(); }

/// \brief The evaluated value of an integer constant expression.
struct ICEValue {
  int64_t value = 0;
  bool is_unsigned = false;
};

/// \brief Performs all semantic analysis and AST construction. The parser
///        calls the ActOn* methods as it recognizes grammar productions; Sema
///        checks constraints, applies conversions, and builds nodes in the
///        ASTContext (mirrors Clang's Sema).
class Sema {
 public:
  Sema(Preprocessor& pp, ASTContext& ctx);

  Sema(const Sema&) = delete;
  Sema& operator=(const Sema&) = delete;

  ~Sema();

  ASTContext& GetASTContext() noexcept { return ctx_; }
  DiagnosticsEngine& GetDiagnostics() noexcept { return diags_; }
  Preprocessor& GetPreprocessor() noexcept { return pp_; }

  DiagnosticBuilder Diag(SourceLocation loc, diag::DiagKind kind) {
    return diags_.Report(loc, kind);
  }

  //===--------------------------------------------------------------------===//
  // Scope management. The parser owns scope push/pop order; Sema owns the
  // name bindings hanging off each scope.
  //===--------------------------------------------------------------------===//

  void PushScope(Scope* s) noexcept { cur_scope_ = s; }
  void PopScope(Scope* s);
  Scope* GetCurScope() const noexcept { return cur_scope_; }

  //===--------------------------------------------------------------------===//
  // Name lookup.
  //===--------------------------------------------------------------------===//

  /// \brief Innermost visible declaration of \p name in the ordinary
  ///        namespace, or null.
  NamedDecl* LookupOrdinaryName(const IdentifierInfo* name) const;

  /// \brief Innermost visible declaration of \p name in the tag namespace.
  TagDecl* LookupTagName(const IdentifierInfo* name) const;

  /// \brief True if \p name currently names a typedef — the parser's
  ///        typedef-name disambiguation hook.
  bool IsTypeName(const IdentifierInfo* name) const;

  //===--------------------------------------------------------------------===//
  // Declarations (sema_decl.cc).
  //===--------------------------------------------------------------------===//

  /// \brief Processes one init-declarator's declarator (before any
  ///        initializer): builds the type, checks redeclarations, creates and
  ///        registers the Var/Typedef/FunctionDecl.
  DeclResult ActOnDeclarator(Scope* s, Declarator& d);

  /// \brief Attaches and checks \p init (already parsed) on \p decl.
  void AddInitializerToDecl(Decl* decl, Expr* init);

  /// \brief Final per-declarator checks that depend on the presence/absence
  ///        of an initializer (incomplete types, array size deduction...).
  void FinalizeDeclaration(Decl* decl);

  /// \brief Emits a warning if a specifiers-only declaration declares nothing
  ///        (`int;`), and returns the tag decl if it declares one.
  DeclResult ActOnEmptyDeclaration(Scope* s, const DeclSpec& ds,
                                   SourceLocation semi_loc);

  ParmVarDecl* ActOnParamDeclarator(Scope* s, Declarator& d);

  FunctionDecl* ActOnStartOfFunctionDef(Scope* fn_scope, Declarator& d);
  void ActOnFinishFunctionBody(FunctionDecl* fd, Stmt* body);

  /// How a tag specifier is used syntactically.
  enum class TagUseKind : uint8_t {
    kReference,    ///< `struct S x;` — reference or implicit declaration
    kDeclaration,  ///< `struct S;` — standalone forward declaration
    kDefinition,   ///< `struct S { ... }`
  };

  TagDecl* ActOnTag(Scope* s, TagKind kind, TagUseKind use,
                    SourceLocation kw_loc, const IdentifierInfo* name,
                    SourceLocation name_loc);
  void ActOnTagStartDefinition(TagDecl* tag);
  FieldDecl* ActOnField(Scope* s, RecordDecl* record, Declarator& d,
                        Expr* bitfield_width);
  void ActOnTagFinishDefinition(TagDecl* tag, SourceLocation rbrace_loc);

  EnumConstantDecl* ActOnEnumConstant(Scope* s, EnumDecl* enum_decl,
                                      EnumConstantDecl* last,
                                      SourceLocation id_loc,
                                      const IdentifierInfo* name,
                                      Expr* value_expr);

  DeclResult ActOnStaticAssert(SourceLocation loc, Expr* cond,
                               std::string message);

  /// \brief Runs end-of-TU checks: incomplete tentative definitions.
  void ActOnEndOfTranslationUnit();

  //===--------------------------------------------------------------------===//
  // Types (sema_type.cc).
  //===--------------------------------------------------------------------===//

  /// \brief Builds the base type written by a DeclSpec, diagnosing invalid
  ///        specifier combinations.
  QualType ConvertDeclSpecToType(const DeclSpec& ds);

  /// \brief Wraps the DeclSpec type in the declarator's pointer/array/
  ///        function chunks, diagnosing invalid compositions. Returns a null
  ///        QualType on error.
  QualType GetTypeForDeclarator(Declarator& d);

  /// \brief `(type-name)` handling for casts, sizeof, compound literals.
  QualType ActOnTypeName(Declarator& d);

  /// \brief Diagnoses if \p t is incomplete; returns true on error.
  bool RequireCompleteType(SourceLocation loc, QualType t,
                           diag::DiagKind kind);

  //===--------------------------------------------------------------------===//
  // Expressions (sema_expr.cc).
  //===--------------------------------------------------------------------===//

  ExprResult ActOnIdExpression(Scope* s, const IdentifierInfo* name,
                               SourceLocation loc, bool is_callee);
  ExprResult ActOnNumericConstant(const Token& tok);
  ExprResult ActOnCharacterConstant(const Token& tok);
  ExprResult ActOnStringLiteral(const std::vector<Token>& toks);
  ExprResult ActOnParenExpr(SourceLocation lparen, SourceLocation rparen,
                            Expr* sub);
  ExprResult ActOnUnaryOp(SourceLocation op_loc, UnaryOperatorKind op,
                          Expr* operand);
  ExprResult ActOnBinOp(SourceLocation op_loc, BinaryOperatorKind op,
                        Expr* lhs, Expr* rhs);
  ExprResult ActOnConditionalOp(SourceLocation question_loc,
                                SourceLocation colon_loc, Expr* cond,
                                Expr* lhs, Expr* rhs);
  ExprResult ActOnArraySubscript(Expr* base, SourceLocation lsquare, Expr* idx,
                                 SourceLocation rsquare);
  ExprResult ActOnCallExpr(Expr* callee, std::vector<Expr*> args,
                           SourceLocation rparen);
  ExprResult ActOnMemberAccess(Expr* base, SourceLocation op_loc,
                               bool is_arrow, const IdentifierInfo* name,
                               SourceLocation name_loc);
  ExprResult ActOnCastExpr(SourceLocation lparen, QualType type,
                           SourceLocation rparen, Expr* operand);
  ExprResult ActOnCompoundLiteral(SourceLocation lparen, QualType type,
                                  SourceLocation rparen, Expr* init);
  ExprResult ActOnSizeofAlignof(SourceLocation op_loc, bool is_sizeof,
                                QualType type, Expr* operand,
                                SourceRange range);

  struct GenericAssoc {
    QualType type;  ///< null for `default:`
    SourceLocation loc;
    Expr* expr;
  };
  ExprResult ActOnGenericSelection(SourceLocation generic_loc,
                                   Expr* controlling,
                                   std::vector<GenericAssoc> assocs,
                                   SourceLocation rparen);

  ExprResult ActOnInitList(SourceLocation lbrace, std::vector<Expr*> inits,
                           SourceLocation rbrace);
  ExprResult ActOnDesignatedInit(std::vector<Designator> designators,
                                 Expr* init);

  //===--------------------------------------------------------------------===//
  // Conversions (sema_expr.cc). Public for use by initialization checking.
  //===--------------------------------------------------------------------===//

  /// \brief Wraps \p e in an ImplicitCastExpr to \p type (no-op if the type
  ///        is already identical).
  Expr* ImpCastExprToType(Expr* e, QualType type, CastKind kind);

  /// \brief Array-to-pointer and function-to-pointer decay plus
  ///        lvalue-to-rvalue conversion (C11 6.3.2.1).
  ExprResult DefaultFunctionArrayLvalueConversion(Expr* e);

  /// \brief DefaultFunctionArrayLvalueConversion + integer promotions.
  ExprResult UsualUnaryConversions(Expr* e);

  /// \brief The usual arithmetic conversions (C11 6.3.1.8); converts both
  ///        operands in place and returns the common type (null on error).
  QualType UsualArithmeticConversions(Expr*& lhs, Expr*& rhs);

  /// \brief Default argument promotions for variadic/unprototyped calls
  ///        (C11 6.5.2.2p6): float->double on top of the usual unary set.
  ExprResult DefaultArgumentPromotion(Expr* e);

  /// The verdict of assignment-compatibility checking (C11 6.5.16.1p1).
  enum class AssignConvertType : uint8_t {
    kCompatible,
    kPointerInt,          ///< pointer <- int or int <- pointer (warning)
    kIncompatiblePointer, ///< distinct pointee types (warning)
    kDiscardsQualifiers,  ///< pointee qualifiers dropped (warning)
    kIncompatible,        ///< hard error
  };

  /// \brief Checks and converts \p rhs for assignment to \p lhs_type,
  ///        applying the needed implicit cast on success/warning verdicts.
  ///        \p rhs_type_out receives the RHS type after lvalue conversion but
  ///        before the assignment conversion — the type to name in
  ///        diagnostics.
  AssignConvertType CheckSingleAssignmentConstraints(
      QualType lhs_type, Expr*& rhs, QualType* rhs_type_out = nullptr);

  /// \brief Emits the diagnostic for a non-kCompatible verdict. \p action is
  ///        the context: "initializing", "assigning to", "passing",
  ///        "returning".
  void DiagnoseAssignmentResult(AssignConvertType result, SourceLocation loc,
                                QualType lhs_type, QualType rhs_type,
                                std::string_view action);

  /// \brief Converts a controlling expression to a scalar "boolean" condition
  ///        (if/while/for/!/&&/||), diagnosing non-scalar operands.
  ExprResult CheckBooleanCondition(Expr* e, SourceLocation stmt_loc);

  //===--------------------------------------------------------------------===//
  // Statements (sema_stmt.cc).
  //===--------------------------------------------------------------------===//

  StmtResult ActOnNullStmt(SourceLocation semi_loc);
  StmtResult ActOnCompoundStmt(std::vector<Stmt*> body, SourceRange braces);
  StmtResult ActOnDeclStmt(std::vector<Decl*> decls, SourceRange range);
  StmtResult ActOnExprStmt(Expr* e);
  StmtResult ActOnIfStmt(SourceLocation if_loc, Expr* cond, Stmt* then_stmt,
                         Stmt* else_stmt);
  StmtResult ActOnWhileStmt(SourceLocation while_loc, Expr* cond, Stmt* body);
  StmtResult ActOnDoStmt(SourceLocation do_loc, Stmt* body, Expr* cond,
                         SourceLocation rparen);
  StmtResult ActOnForStmt(SourceLocation for_loc, Stmt* init, Expr* cond,
                          Expr* inc, Stmt* body);
  StmtResult ActOnStartOfSwitchStmt(SourceLocation switch_loc, Expr* cond);
  StmtResult ActOnFinishSwitchStmt(Stmt* switch_stmt, Stmt* body);
  StmtResult ActOnCaseStmt(SourceLocation case_loc, Expr* value,
                           SourceLocation colon_loc);
  void ActOnCaseStmtBody(Stmt* case_stmt, Stmt* sub);
  StmtResult ActOnDefaultStmt(SourceLocation default_loc, Stmt* sub);
  StmtResult ActOnBreakStmt(SourceLocation loc, Scope* s);
  StmtResult ActOnContinueStmt(SourceLocation loc, Scope* s);
  StmtResult ActOnReturnStmt(SourceLocation loc, Expr* value);
  StmtResult ActOnGotoStmt(SourceLocation goto_loc,
                           const IdentifierInfo* label,
                           SourceLocation label_loc);
  StmtResult ActOnLabelStmt(SourceLocation label_loc,
                            const IdentifierInfo* label, Stmt* sub);

  //===--------------------------------------------------------------------===//
  // Initialization (sema_init.cc).
  //===--------------------------------------------------------------------===//

  /// \brief Checks \p init as an initializer for \p type. On success returns
  ///        the (possibly converted / semantic-form) initializer and, for an
  ///        incomplete array type, completes \p type from the initializer.
  ExprResult CheckInitializer(QualType& type, Expr* init,
                              bool is_static_storage);

  //===--------------------------------------------------------------------===//
  // Constant expressions (const_eval.cc).
  //===--------------------------------------------------------------------===//

  /// \brief Evaluates an integer constant expression (C11 6.6p6). Returns
  ///        nullopt (without diagnosing) if not constant.
  std::optional<ICEValue> EvaluateICE(const Expr* e) const;

  /// \brief Like EvaluateICE but diagnoses err_expr_not_ice on failure.
  std::optional<ICEValue> VerifyICE(const Expr* e, SourceLocation loc,
                                    diag::DiagKind kind = diag::err_expr_not_ice);

 private:
  //===--------------------------------------------------------------------===//
  // Lookup internals (sema.cc).
  //===--------------------------------------------------------------------===//

  friend class LookupChains;

  /// \brief Registers \p d in the ordinary namespace of scope \p s.
  void PushOrdinaryDecl(Scope* s, NamedDecl* d);
  void PushTagDecl(Scope* s, TagDecl* d);

  /// \brief The innermost binding of \p name declared *directly in* \p s, or
  ///        null — the redeclaration-in-same-scope test.
  NamedDecl* LookupOrdinaryNameInScope(const IdentifierInfo* name,
                                       Scope* s) const;
  TagDecl* LookupTagNameInScope(const IdentifierInfo* name, Scope* s) const;

  //===--------------------------------------------------------------------===//
  // Declaration internals (sema_decl.cc).
  //===--------------------------------------------------------------------===//

  DeclResult ActOnFunctionDeclarator(Scope* s, Declarator& d, QualType type,
                                     StorageClass sc);
  DeclResult ActOnVariableDeclarator(Scope* s, Declarator& d, QualType type,
                                     StorageClass sc);
  DeclResult ActOnTypedefDeclarator(Scope* s, Declarator& d, QualType type);

  /// \brief Checks an enum constant's value expression; returns its value.
  std::optional<int64_t> CheckEnumConstantValue(Expr* value_expr,
                                                SourceLocation loc);

  LabelDecl* LookupOrCreateLabel(const IdentifierInfo* name,
                                 SourceLocation loc);

  //===--------------------------------------------------------------------===//
  // Expression internals (sema_expr.cc).
  //===--------------------------------------------------------------------===//

  QualType CheckMultiplyDivideOperands(Expr*& lhs, Expr*& rhs,
                                       SourceLocation loc, bool is_div);
  QualType CheckRemainderOperands(Expr*& lhs, Expr*& rhs, SourceLocation loc);
  QualType CheckAdditionOperands(Expr*& lhs, Expr*& rhs, SourceLocation loc);
  QualType CheckSubtractionOperands(Expr*& lhs, Expr*& rhs,
                                    SourceLocation loc);
  QualType CheckShiftOperands(Expr*& lhs, Expr*& rhs, SourceLocation loc);
  QualType CheckCompareOperands(Expr*& lhs, Expr*& rhs, SourceLocation loc,
                                BinaryOperatorKind op);
  QualType CheckBitwiseOperands(Expr*& lhs, Expr*& rhs, SourceLocation loc);
  QualType CheckLogicalOperands(Expr*& lhs, Expr*& rhs, SourceLocation loc);
  QualType CheckAssignmentOperands(Expr* lhs, Expr*& rhs, SourceLocation loc);

  /// \brief Verifies that \p e is a modifiable lvalue (for assignment and
  ///        ++/--); emits the appropriate diagnostic if not.
  bool CheckModifiableLValue(Expr* e, SourceLocation loc);

  QualType CheckIncrementDecrementOperand(Expr*& operand, SourceLocation loc,
                                          bool is_increment);
  ExprResult CheckAddressOfOperand(Expr* operand, SourceLocation loc);
  ExprResult CheckIndirectionOperand(Expr* operand, SourceLocation loc);

  void DiagnoseBadBinaryOperands(SourceLocation loc, const Expr* lhs,
                                 const Expr* rhs);

  /// \brief The CastKind that converts scalar \p from to scalar \p to.
  CastKind GetScalarCastKind(QualType from, QualType to);

  //===--------------------------------------------------------------------===//
  // Members.
  //===--------------------------------------------------------------------===//

  Preprocessor& pp_;
  ASTContext& ctx_;
  DiagnosticsEngine& diags_;

  Scope* cur_scope_ = nullptr;

  /// Ordinary- and tag-namespace binding stacks per identifier: innermost
  /// binding last, each tagged with the scope that owns it.
  template <typename D>
  using Chains =
      std::unordered_map<const IdentifierInfo*,
                         std::vector<std::pair<Scope*, D*>>>;
  Chains<NamedDecl> ordinary_names_;
  Chains<TagDecl> tag_names_;

  //===--------------------------------------------------------------------===//
  // Per-function state.
  //===--------------------------------------------------------------------===//

  FunctionDecl* cur_function_ = nullptr;

  /// Labels of the current function, by name.
  std::unordered_map<const IdentifierInfo*, LabelDecl*> function_labels_;
  /// Goto statements seen, for undefined-label diagnostics at function end.
  std::vector<std::pair<const LabelDecl*, SourceLocation>> gotos_;

  /// One entry per lexically-nested switch statement being parsed.
  struct SwitchInfo {
    SwitchStmt* stmt;
    QualType cond_type;  ///< promoted condition type
    std::map<int64_t, const CaseStmt*> case_values;
    const DefaultStmt* default_stmt = nullptr;
  };
  std::vector<SwitchInfo> switch_stack_;

  /// File-scope tentative definitions (C11 6.9.2p2), checked at end of TU.
  std::vector<VarDecl*> tentative_definitions_;

  /// Tags whose member list is currently being parsed, for nested-
  /// redefinition detection.
  std::vector<const TagDecl*> tags_being_defined_;
};

}  // namespace bcc
