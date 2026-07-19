#include <cassert>

#include "bcc/codegen/codegen_function.hh"

namespace bcc::codegen {

void CodeGenFunction::EmitStmt(const Stmt* s) {
  if (!s) return;

  if (s->IsExpr()) {
    EmitIgnoredExpr(s->As<Expr>());
    return;
  }

  switch (s->GetStmtClass()) {
    case StmtClass::kNullStmt:
      return;
    case StmtClass::kCompoundStmt:
      EmitCompoundStmt(s->As<CompoundStmt>());
      return;
    case StmtClass::kDeclStmt:
      for (const Decl* d : s->As<DeclStmt>()->GetDecls()) EmitDecl(d);
      return;
    case StmtClass::kIfStmt:
      EmitIfStmt(s->As<IfStmt>());
      return;
    case StmtClass::kWhileStmt:
      EmitWhileStmt(s->As<WhileStmt>());
      return;
    case StmtClass::kDoStmt:
      EmitDoStmt(s->As<DoStmt>());
      return;
    case StmtClass::kForStmt:
      EmitForStmt(s->As<ForStmt>());
      return;
    case StmtClass::kSwitchStmt:
      EmitSwitchStmt(s->As<SwitchStmt>());
      return;
    case StmtClass::kCaseStmt:
      EmitCaseStmt(s->As<CaseStmt>());
      return;
    case StmtClass::kDefaultStmt:
      EmitDefaultStmt(s->As<DefaultStmt>());
      return;
    case StmtClass::kBreakStmt:
      EmitBreak();
      return;
    case StmtClass::kContinueStmt:
      EmitContinue();
      return;
    case StmtClass::kReturnStmt:
      EmitReturnStmt(s->As<ReturnStmt>());
      return;
    case StmtClass::kGotoStmt:
      EmitGotoStmt(s->As<GotoStmt>());
      return;
    case StmtClass::kLabelStmt:
      EmitLabelStmt(s->As<LabelStmt>());
      return;
    default:
      assert(false && "unhandled statement class");
      return;
  }
}

void CodeGenFunction::EmitCompoundStmt(const CompoundStmt* s) {
  for (const Stmt* sub : s->GetBody()) EmitStmt(sub);
}

void CodeGenFunction::EmitIfStmt(const IfStmt* s) {
  ir::BasicBlock* then_bb = CreateBlock("if.then");
  ir::BasicBlock* end_bb = CreateBlock("if.end");
  ir::BasicBlock* else_bb = s->GetElse() ? CreateBlock("if.else") : end_bb;

  EmitBranchOnBool(s->GetCond(), then_bb, else_bb);

  EmitBlock(then_bb);
  EmitStmt(s->GetThen());
  if (HaveInsertPoint()) builder_.CreateBr(end_bb);

  if (s->GetElse()) {
    EmitBlock(else_bb);
    EmitStmt(s->GetElse());
  }
  EmitBlock(end_bb);
}

void CodeGenFunction::EmitWhileStmt(const WhileStmt* s) {
  ir::BasicBlock* cond_bb = CreateBlock("while.cond");
  ir::BasicBlock* body_bb = CreateBlock("while.body");
  ir::BasicBlock* end_bb = CreateBlock("while.end");

  EmitBlock(cond_bb);
  EmitBranchOnBool(s->GetCond(), body_bb, end_bb);

  EmitBlock(body_bb);
  break_continue_stack_.push_back({end_bb, cond_bb});
  EmitStmt(s->GetBody());
  break_continue_stack_.pop_back();
  if (HaveInsertPoint()) builder_.CreateBr(cond_bb);

  EmitBlock(end_bb);
}

void CodeGenFunction::EmitDoStmt(const DoStmt* s) {
  ir::BasicBlock* body_bb = CreateBlock("do.body");
  ir::BasicBlock* cond_bb = CreateBlock("do.cond");
  ir::BasicBlock* end_bb = CreateBlock("do.end");

  EmitBlock(body_bb);
  break_continue_stack_.push_back({end_bb, cond_bb});
  EmitStmt(s->GetBody());
  break_continue_stack_.pop_back();

  EmitBlock(cond_bb);
  EmitBranchOnBool(s->GetCond(), body_bb, end_bb);

  EmitBlock(end_bb);
}

void CodeGenFunction::EmitForStmt(const ForStmt* s) {
  if (s->GetInit()) EmitStmt(s->GetInit());

  ir::BasicBlock* cond_bb = CreateBlock("for.cond");
  ir::BasicBlock* body_bb = CreateBlock("for.body");
  ir::BasicBlock* end_bb = CreateBlock("for.end");
  ir::BasicBlock* inc_bb = s->GetInc() ? CreateBlock("for.inc") : nullptr;
  ir::BasicBlock* continue_bb = inc_bb ? inc_bb : cond_bb;

  EmitBlock(cond_bb);
  if (s->GetCond()) {
    EmitBranchOnBool(s->GetCond(), body_bb, end_bb);
  } else {
    builder_.CreateBr(body_bb);
  }

  EmitBlock(body_bb);
  break_continue_stack_.push_back({end_bb, continue_bb});
  EmitStmt(s->GetBody());
  break_continue_stack_.pop_back();

  if (inc_bb) {
    EmitBlock(inc_bb);
    EmitIgnoredExpr(s->GetInc());
  }
  if (HaveInsertPoint()) builder_.CreateBr(cond_bb);

  EmitBlock(end_bb);
}

void CodeGenFunction::EmitSwitchStmt(const SwitchStmt* s) {
  const ir::Value* cond = EmitScalarExpr(s->GetCond());
  ir::BasicBlock* epilog = CreateBlock("sw.epilog");

  ir::SwitchInst* sw = builder_.CreateSwitch(cond, epilog);
  switch_stack_.push_back(sw);

  // continue passes through a switch to the enclosing loop.
  ir::BasicBlock* outer_continue =
      break_continue_stack_.empty()
          ? nullptr
          : break_continue_stack_.back().continue_block;
  break_continue_stack_.push_back({epilog, outer_continue});

  // Statements before the first case label are unreachable; give them a
  // block so emission has somewhere to go.
  EmitBlock(CreateBlock("sw.body"));
  EmitStmt(s->GetBody());

  break_continue_stack_.pop_back();
  switch_stack_.pop_back();

  EmitBlock(epilog);
}

void CodeGenFunction::EmitCaseStmt(const CaseStmt* s) {
  assert(!switch_stack_.empty() && "case outside switch");
  ir::SwitchInst* sw = switch_stack_.back();

  // Adjacent case blocks fall through naturally: EmitBlock adds the branch
  // from the previous (unterminated) block.
  ir::BasicBlock* bb = CreateBlock("sw.bb");
  EmitBlock(bb);

  const auto* cond_type =
      sw->GetCondition()->GetType()->As<ir::IntegerType>();
  sw->AddCase(Ir().GetInt(cond_type, static_cast<uint64_t>(s->GetValue())),
              bb);
  EmitStmt(s->GetSubStmt());
}

void CodeGenFunction::EmitDefaultStmt(const DefaultStmt* s) {
  assert(!switch_stack_.empty() && "default outside switch");
  ir::BasicBlock* bb = CreateBlock("sw.default");
  EmitBlock(bb);
  switch_stack_.back()->SetDefaultDest(bb);
  EmitStmt(s->GetSubStmt());
}

void CodeGenFunction::EmitReturnStmt(const ReturnStmt* s) {
  if (const Expr* value = s->GetValue()) {
    QualType t = value->GetType();
    if (t->IsAggregateType() || t->IsUnionType()) {
      EmitAggExpr(value, return_value_);
    } else if (t->IsVoidType()) {
      EmitIgnoredExpr(value);
    } else {
      const ir::Value* v = EmitScalarExpr(value);
      if (v && return_value_.IsValid()) {
        builder_.CreateStore(v, return_value_.ptr, return_value_.align);
      }
    }
  }
  builder_.CreateBr(return_block_);
  // Anything after `return` is dead but still emitted (Clang's model).
  EmitBlock(CreateBlock(""));
}

void CodeGenFunction::EmitBreak() {
  assert(!break_continue_stack_.empty() && "break outside loop/switch");
  builder_.CreateBr(break_continue_stack_.back().break_block);
  EmitBlock(CreateBlock(""));
}

void CodeGenFunction::EmitContinue() {
  ir::BasicBlock* target = nullptr;
  for (auto it = break_continue_stack_.rbegin();
       it != break_continue_stack_.rend(); ++it) {
    if (it->continue_block) {
      target = it->continue_block;
      break;
    }
  }
  assert(target && "continue outside loop");
  builder_.CreateBr(target);
  EmitBlock(CreateBlock(""));
}

void CodeGenFunction::EmitGotoStmt(const GotoStmt* s) {
  builder_.CreateBr(GetBlockForLabel(s->GetLabel()));
  EmitBlock(CreateBlock(""));
}

void CodeGenFunction::EmitLabelStmt(const LabelStmt* s) {
  EmitBlock(GetBlockForLabel(s->GetLabel()));
  EmitStmt(s->GetSubStmt());
}

ir::BasicBlock* CodeGenFunction::GetBlockForLabel(const LabelDecl* label) {
  auto it = label_blocks_.find(label);
  if (it != label_blocks_.end()) return it->second;
  ir::BasicBlock* bb = CreateBlock(std::string(label->GetName()));
  label_blocks_.emplace(label, bb);
  return bb;
}

}  // namespace bcc::codegen
