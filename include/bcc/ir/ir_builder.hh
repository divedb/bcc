#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bcc/ir/basic_block.hh"
#include "bcc/ir/instruction.hh"
#include "bcc/ir/ir_context.hh"

namespace bcc::ir {

/// \brief The only way codegen makes instructions: appends to the current
///        insertion block. Allocas are inserted at a dedicated point at the
///        top of the entry block (SetAllocaBlock), so locals group there
///        regardless of where the declaration appears (Clang's
///        AllocaInsertPt).
class IRBuilder {
 public:
  explicit IRBuilder(IRContext& ctx) : ctx_(ctx) {}

  IRContext& GetContext() noexcept { return ctx_; }

  void SetInsertPoint(BasicBlock* bb) noexcept { block_ = bb; }
  BasicBlock* GetInsertBlock() const noexcept { return block_; }

  void SetAllocaBlock(BasicBlock* bb) noexcept {
    alloca_block_ = bb;
    num_allocas_ = 0;
  }

  //===--------------------------------------------------------------------===//
  // Memory.
  //===--------------------------------------------------------------------===//

  AllocaInst* CreateAlloca(const Type* type, uint64_t align,
                           std::string name) {
    auto inst = std::make_unique<AllocaInst>(ctx_.GetPointerType(), type,
                                             align);
    inst->SetName(std::move(name));
    AllocaInst* raw = inst.get();
    alloca_block_->Insert(num_allocas_++, std::move(inst));
    return raw;
  }

  LoadInst* CreateLoad(const Type* type, const Value* ptr, uint64_t align,
                       std::string name = "") {
    return Append(std::make_unique<LoadInst>(type, ptr, align),
                  std::move(name));
  }

  StoreInst* CreateStore(const Value* value, const Value* ptr,
                         uint64_t align) {
    return Append(
        std::make_unique<StoreInst>(ctx_.GetVoidType(), value, ptr, align));
  }

  GEPInst* CreateGEP(const Type* source_type, const Value* base,
                     std::vector<const Value*> indices,
                     std::string name = "") {
    return Append(std::make_unique<GEPInst>(ctx_.GetPointerType(),
                                            source_type, base,
                                            std::move(indices)),
                  std::move(name));
  }

  //===--------------------------------------------------------------------===//
  // Arithmetic, comparison, casts.
  //===--------------------------------------------------------------------===//

  BinaryOperator* CreateBinOp(Opcode opcode, const Value* lhs,
                              const Value* rhs, std::string name = "") {
    return Append(std::make_unique<BinaryOperator>(opcode, lhs->GetType(),
                                                   lhs, rhs),
                  std::move(name));
  }

  CmpInst* CreateICmp(CmpPredicate pred, const Value* lhs, const Value* rhs,
                      std::string name = "") {
    return Append(std::make_unique<CmpInst>(Opcode::kICmp,
                                            ctx_.GetInt1Type(), pred, lhs,
                                            rhs),
                  std::move(name));
  }

  CmpInst* CreateFCmp(CmpPredicate pred, const Value* lhs, const Value* rhs,
                      std::string name = "") {
    return Append(std::make_unique<CmpInst>(Opcode::kFCmp,
                                            ctx_.GetInt1Type(), pred, lhs,
                                            rhs),
                  std::move(name));
  }

  CastInst* CreateCast(Opcode opcode, const Value* value,
                       const Type* dest_type, std::string name = "") {
    return Append(std::make_unique<CastInst>(opcode, dest_type, value),
                  std::move(name));
  }

  //===--------------------------------------------------------------------===//
  // Control flow.
  //===--------------------------------------------------------------------===//

  BranchInst* CreateBr(BasicBlock* dest) {
    return Append(std::make_unique<BranchInst>(ctx_.GetVoidType(), dest));
  }

  BranchInst* CreateCondBr(const Value* cond, BasicBlock* if_true,
                           BasicBlock* if_false) {
    return Append(std::make_unique<BranchInst>(ctx_.GetVoidType(), cond,
                                               if_true, if_false));
  }

  SwitchInst* CreateSwitch(const Value* cond, BasicBlock* dflt) {
    return Append(
        std::make_unique<SwitchInst>(ctx_.GetVoidType(), cond, dflt));
  }

  RetInst* CreateRet(const Value* value) {
    return Append(std::make_unique<RetInst>(ctx_.GetVoidType(), value));
  }
  RetInst* CreateRetVoid() { return CreateRet(nullptr); }

  UnreachableInst* CreateUnreachable() {
    return Append(std::make_unique<UnreachableInst>(ctx_.GetVoidType()));
  }

  PhiNode* CreatePhi(const Type* type, std::string name = "") {
    return Append(std::make_unique<PhiNode>(type), std::move(name));
  }

  CallInst* CreateCall(const FunctionType* fn_type, const Value* callee,
                       std::vector<const Value*> args,
                       std::string name = "") {
    return Append(
        std::make_unique<CallInst>(fn_type, callee, std::move(args)),
        std::move(name));
  }

 private:
  template <typename T>
  T* Append(std::unique_ptr<T> inst, std::string name = "") {
    if (!name.empty()) inst->SetName(std::move(name));
    return static_cast<T*>(block_->Append(std::move(inst)));
  }

  IRContext& ctx_;
  BasicBlock* block_ = nullptr;
  BasicBlock* alloca_block_ = nullptr;
  size_t num_allocas_ = 0;
};

}  // namespace bcc::ir
