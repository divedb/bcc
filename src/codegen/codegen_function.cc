#include "bcc/codegen/codegen_function.hh"

#include <cassert>
#include <utility>

namespace bcc::codegen {

void CodeGenFunction::EmitFunction(const FunctionDecl* fd, ir::Function* fn) {
  fn_ = fn;
  fn_decl_ = fd;

  ir::BasicBlock* entry = CreateBlock("entry");
  EmitBlock(entry);
  builder_.SetAllocaBlock(entry);
  return_block_ = CreateBlock("return");

  QualType ret_type = fd->GetReturnType();
  if (!ret_type->IsVoidType()) {
    return_value_ = Address(
        builder_.CreateAlloca(ConvertType(ret_type),
                              Ast().GetTypeAlign(ret_type), "retval"),
        Ast().GetTypeAlign(ret_type));
  }

  // Parameters: %x.addr allocas; the body only ever sees the alloca.
  const auto& params = fd->GetParams();
  for (unsigned i = 0; i < params.size() && i < fn->GetNumArgs(); ++i) {
    const ParmVarDecl* parm = params[i];
    std::string name(parm->GetName());
    ir::Argument* arg = fn->GetArg(i);
    arg->SetName(name);
    uint64_t align = Ast().GetTypeAlign(parm->GetType());
    ir::Value* slot = builder_.CreateAlloca(ConvertType(parm->GetType()),
                                            align, name + ".addr");
    builder_.CreateStore(arg, slot, align);
    local_addrs_[parm] = Address(slot, align);
  }

  // C11 5.1.2.2.3: main returns 0 on falling off the end.
  if (fd->GetName() == "main" && return_value_.IsValid()) {
    builder_.CreateStore(Ir().GetInt32(0), return_value_.ptr,
                         return_value_.align);
  }

  EmitStmt(fd->GetBody());

  // Fall-through (and the shared epilogue): EmitBlock branches the current
  // block into `return` if it is unterminated.
  EmitBlock(return_block_);
  if (!return_value_.IsValid()) {
    builder_.CreateRetVoid();
  } else {
    const ir::Value* v = builder_.CreateLoad(
        ConvertType(ret_type), return_value_.ptr, return_value_.align);
    builder_.CreateRet(v);
  }
}

ir::BasicBlock* CodeGenFunction::CreateBlock(std::string name) {
  auto bb = std::make_unique<ir::BasicBlock>(std::move(name));
  ir::BasicBlock* raw = bb.get();
  pending_blocks_.emplace(raw, std::move(bb));
  return raw;
}

void CodeGenFunction::EmitBlock(ir::BasicBlock* bb) {
  auto it = pending_blocks_.find(bb);
  assert(it != pending_blocks_.end() && "block emitted twice");
  std::unique_ptr<ir::BasicBlock> owned = std::move(it->second);
  pending_blocks_.erase(it);

  if (HaveInsertPoint()) builder_.CreateBr(bb);
  fn_->AppendBlock(std::move(owned));
  builder_.SetInsertPoint(bb);
}

bool CodeGenFunction::HaveInsertPoint() const noexcept {
  ir::BasicBlock* cur = builder_.GetInsertBlock();
  return cur != nullptr && !cur->IsTerminated();
}

void CodeGenFunction::EmitDecl(const Decl* d) {
  if (const auto* vd = d->As<VarDecl>()) {
    if (!vd->As<ParmVarDecl>()) EmitLocalVarDecl(vd);
  }
  // Typedefs, tags, static asserts: no code.
}

void CodeGenFunction::EmitLocalVarDecl(const VarDecl* vd) {
  if (vd->GetStorageClass() == StorageClass::kStatic) {
    cgm_.CreateStaticLocal(vd, fn_->GetName());
    return;
  }
  if (vd->GetStorageClass() == StorageClass::kExtern) {
    cgm_.GetOrCreateGlobal(vd);
    return;
  }

  QualType t = vd->GetType();
  if (t.GetCanonical().GetTypePtr()->GetTypeClass() ==
      TypeClass::kVariableArray) {
    cgm_.ErrorUnsupported(vd->GetLocation(), "variable length array");
    return;
  }

  uint64_t align = Ast().GetTypeAlign(t);
  ir::Value* slot =
      builder_.CreateAlloca(ConvertType(t), align, std::string(vd->GetName()));
  Address addr(slot, align);
  local_addrs_[vd] = addr;

  if (!vd->HasInit()) return;
  const Expr* init = vd->GetInit();
  if (t->IsAggregateType() || t->IsUnionType()) {
    EmitAggExpr(init, addr);
  } else {
    const ir::Value* v = EmitScalarExpr(init);
    if (v) builder_.CreateStore(v, addr.ptr, addr.align);
  }
}

Address CodeGenFunction::CreateTempAlloca(QualType type, std::string name) {
  uint64_t align = Ast().GetTypeAlign(type);
  return Address(
      builder_.CreateAlloca(ConvertType(type), align, std::move(name)),
      align);
}

}  // namespace bcc::codegen
