#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "bcc/ast/decl.hh"
#include "bcc/ast/expr.hh"
#include "bcc/ast/stmt.hh"
#include "bcc/codegen/codegen_module.hh"
#include "bcc/codegen/codegen_value.hh"
#include "bcc/ir/function.hh"
#include "bcc/ir/ir_builder.hh"

namespace bcc::codegen {

/// \brief Per-function lowering state (Clang's CodeGenFunction): insertion
///        point, entry-block alloca placement, the shared ReturnBlock +
///        ReturnValue slot, break/continue and switch stacks, and the
///        statement/expression emitters.
class CodeGenFunction {
 public:
  explicit CodeGenFunction(CodeGenModule& cgm)
      : cgm_(cgm), builder_(cgm.GetIRContext()) {}

  void EmitFunction(const FunctionDecl* fd, ir::Function* fn);

  //===--------------------------------------------------------------------===//
  // Blocks.
  //===--------------------------------------------------------------------===//

  /// Creates an unattached block (appended to the function by EmitBlock).
  ir::BasicBlock* CreateBlock(std::string name);

  /// Appends \p bb to the function and makes it the insertion point; if the
  /// current block falls through, adds the fall-through branch first.
  void EmitBlock(ir::BasicBlock* bb);

  /// True if the current block already has a terminator (we are emitting
  /// dead code).
  bool HaveInsertPoint() const noexcept;

  //===--------------------------------------------------------------------===//
  // Statements (codegen_stmt.cc).
  //===--------------------------------------------------------------------===//

  void EmitStmt(const Stmt* s);

  //===--------------------------------------------------------------------===//
  // Declarations inside the function.
  //===--------------------------------------------------------------------===//

  void EmitDecl(const Decl* d);
  void EmitLocalVarDecl(const VarDecl* vd);

  //===--------------------------------------------------------------------===//
  // Expressions (codegen_expr.cc).
  //===--------------------------------------------------------------------===//

  LValue EmitLValue(const Expr* e);
  const ir::Value* EmitScalarExpr(const Expr* e);
  void EmitAggExpr(const Expr* e, Address dest);
  RValue EmitAnyExpr(const Expr* e);
  void EmitIgnoredExpr(const Expr* e);
  void EmitBranchOnBool(const Expr* cond, ir::BasicBlock* true_bb,
                        ir::BasicBlock* false_bb);

  /// Loads a scalar rvalue from \p lv (handles _Bool i8 and bit-fields).
  const ir::Value* EmitLoadOfLValue(LValue lv);
  /// Stores scalar \p value into \p lv; for bit-fields returns the value as
  /// truncated to the field width (the value of the assignment), otherwise
  /// returns \p value.
  const ir::Value* EmitStoreOfScalar(const ir::Value* value, LValue lv);

  /// value != 0 as i1 (for branch conditions).
  const ir::Value* EmitScalarToBool(const ir::Value* v, QualType type);

  /// Value conversion between scalar C types (used by compound assignment
  /// and ++/--, where Sema recorded types rather than cast nodes).
  const ir::Value* EmitScalarConversion(const ir::Value* v, QualType from,
                                        QualType to);

  /// Aggregate copy: load/store of the aggregate IR type.
  void EmitAggregateCopy(Address dest, Address src, QualType type);

  Address CreateTempAlloca(QualType type, std::string name);

 private:
  friend class StmtEmitter;

  // Statement helpers (codegen_stmt.cc).
  void EmitCompoundStmt(const CompoundStmt* s);
  void EmitIfStmt(const IfStmt* s);
  void EmitWhileStmt(const WhileStmt* s);
  void EmitDoStmt(const DoStmt* s);
  void EmitForStmt(const ForStmt* s);
  void EmitSwitchStmt(const SwitchStmt* s);
  void EmitCaseStmt(const CaseStmt* s);
  void EmitDefaultStmt(const DefaultStmt* s);
  void EmitReturnStmt(const ReturnStmt* s);
  void EmitGotoStmt(const GotoStmt* s);
  void EmitLabelStmt(const LabelStmt* s);
  void EmitBreak();
  void EmitContinue();
  ir::BasicBlock* GetBlockForLabel(const LabelDecl* label);

  // Expression helpers (codegen_expr.cc).
  LValue EmitDeclRefLValue(const DeclRefExpr* e);
  LValue EmitMemberExprLValue(const MemberExpr* e);
  const ir::Value* EmitCastExpr(const CastExpr* e);
  const ir::Value* EmitBinaryOperator(const BinaryOperator* e);
  const ir::Value* EmitUnaryOperator(const UnaryOperator* e);
  const ir::Value* EmitScalarArith(BinaryOperatorKind op, const ir::Value* lhs,
                                   const ir::Value* rhs, QualType type,
                                   QualType rhs_ast_type);
  /// Comparison as a raw i1 (no zext) — shared by value and branch forms.
  const ir::Value* EmitCompareI1(const BinaryOperator* e);
  const ir::Value* EmitLogicalOp(const BinaryOperator* e);
  const ir::Value* EmitScalarConditional(const ConditionalOperator* e);
  const ir::Value* EmitIncDec(const UnaryOperator* e);
  /// Emits the call itself; result is the raw call value (may be aggregate).
  const ir::Value* EmitCallRaw(const CallExpr* e);
  void EmitAggInitList(const InitListExpr* e, Address dest);
  const ir::Value* EmitPointerAdd(const ir::Value* ptr, QualType ptr_type,
                                  const ir::Value* idx, QualType idx_type,
                                  bool negate);
  /// Extends/truncates an index value to i64 for GEP.
  const ir::Value* EmitIndexAsI64(const ir::Value* idx, QualType idx_type);

  const ir::Type* ConvertType(QualType t) { return cgm_.GetTypes().Convert(t); }
  ASTContext& Ast() noexcept { return cgm_.GetASTContext(); }
  ir::IRContext& Ir() noexcept { return cgm_.GetIRContext(); }

  struct BreakContinue {
    ir::BasicBlock* break_block = nullptr;
    ir::BasicBlock* continue_block = nullptr;  ///< null inside switch
  };

  CodeGenModule& cgm_;
  ir::IRBuilder builder_;
  ir::Function* fn_ = nullptr;
  const FunctionDecl* fn_decl_ = nullptr;

  ir::BasicBlock* return_block_ = nullptr;
  Address return_value_;  ///< invalid for void functions

  /// Blocks created but not yet appended to the function.
  std::map<ir::BasicBlock*, std::unique_ptr<ir::BasicBlock>> pending_blocks_;

  std::map<const VarDecl*, Address> local_addrs_;
  std::map<const LabelDecl*, ir::BasicBlock*> label_blocks_;
  std::vector<BreakContinue> break_continue_stack_;
  std::vector<ir::SwitchInst*> switch_stack_;
};

}  // namespace bcc::codegen
