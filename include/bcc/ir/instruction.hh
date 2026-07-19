#pragma once

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "bcc/ir/value.hh"

namespace bcc::ir {

class BasicBlock;
class Function;

enum class Opcode : uint8_t {
  // Memory
  kAlloca,
  kLoad,
  kStore,
  kGEP,
  // Integer arithmetic / bitwise (binary)
  kAdd,
  kSub,
  kMul,
  kSDiv,
  kUDiv,
  kSRem,
  kURem,
  kShl,
  kLShr,
  kAShr,
  kAnd,
  kOr,
  kXor,
  // FP arithmetic (binary)
  kFAdd,
  kFSub,
  kFMul,
  kFDiv,
  // Compare
  kICmp,
  kFCmp,
  // Casts (unary, with destination type)
  kTrunc,
  kZExt,
  kSExt,
  kFPTrunc,
  kFPExt,
  kFPToSI,
  kFPToUI,
  kSIToFP,
  kUIToFP,
  kPtrToInt,
  kIntToPtr,
  // Control
  kBr,
  kCondBr,
  kSwitch,
  kRet,
  kUnreachable,
  kPhi,
  // Other
  kCall,
};

/// The LLVM mnemonic for \p op ("add", "icmp", ...; kCondBr prints as "br").
std::string_view GetOpcodeName(Opcode op) noexcept;

/// \brief Base of all instructions. Operands are unowned Value pointers
///        (everything is owned by the IRContext, a Function, or a
///        BasicBlock); there are no use lists — this IR has no optimizer.
class Instruction : public Value {
 public:
  Opcode GetOpcode() const noexcept { return opcode_; }
  BasicBlock* GetParent() const noexcept { return parent_; }
  void SetParent(BasicBlock* bb) noexcept { parent_ = bb; }

  const std::vector<const Value*>& GetOperands() const noexcept {
    return operands_;
  }
  const Value* GetOperand(unsigned i) const noexcept { return operands_[i]; }
  unsigned GetNumOperands() const noexcept {
    return static_cast<unsigned>(operands_.size());
  }

  bool IsTerminator() const noexcept {
    return opcode_ == Opcode::kBr || opcode_ == Opcode::kCondBr ||
           opcode_ == Opcode::kSwitch || opcode_ == Opcode::kRet ||
           opcode_ == Opcode::kUnreachable;
  }
  bool IsBinaryOp() const noexcept {
    return opcode_ >= Opcode::kAdd && opcode_ <= Opcode::kFDiv;
  }
  bool IsCast() const noexcept {
    return opcode_ >= Opcode::kTrunc && opcode_ <= Opcode::kIntToPtr;
  }

  static bool ClassOf(const Value* v) noexcept {
    return v->GetValueKind() == ValueKind::kInstruction;
  }

 protected:
  Instruction(Opcode opcode, const Type* type,
              std::vector<const Value*> operands)
      : Value(ValueKind::kInstruction, type), opcode_(opcode),
        operands_(std::move(operands)) {}

  void AddOperand(const Value* v) { operands_.push_back(v); }
  void SetOperand(unsigned i, const Value* v) { operands_[i] = v; }

 private:
  Opcode opcode_;
  BasicBlock* parent_ = nullptr;
  std::vector<const Value*> operands_;
};

/// \brief `alloca T, align A`. Result type is `ptr`.
class AllocaInst final : public Instruction {
 public:
  AllocaInst(const Type* ptr_type, const Type* allocated_type, uint64_t align)
      : Instruction(Opcode::kAlloca, ptr_type, {}),
        allocated_type_(allocated_type), align_(align) {}

  const Type* GetAllocatedType() const noexcept { return allocated_type_; }
  uint64_t GetAlign() const noexcept { return align_; }

  static bool ClassOf(const Value* v) noexcept {
    const auto* i = v->As<Instruction>();
    return i && i->GetOpcode() == Opcode::kAlloca;
  }

 private:
  const Type* allocated_type_;
  uint64_t align_;
};

/// \brief `load T, ptr P, align A`.
class LoadInst final : public Instruction {
 public:
  LoadInst(const Type* loaded_type, const Value* ptr, uint64_t align)
      : Instruction(Opcode::kLoad, loaded_type, {ptr}), align_(align) {}

  const Value* GetPointer() const noexcept { return GetOperand(0); }
  uint64_t GetAlign() const noexcept { return align_; }

  static bool ClassOf(const Value* v) noexcept {
    const auto* i = v->As<Instruction>();
    return i && i->GetOpcode() == Opcode::kLoad;
  }

 private:
  uint64_t align_;
};

/// \brief `store T V, ptr P, align A`. Produces no value (type void).
class StoreInst final : public Instruction {
 public:
  StoreInst(const Type* void_type, const Value* value, const Value* ptr,
            uint64_t align)
      : Instruction(Opcode::kStore, void_type, {value, ptr}), align_(align) {}

  const Value* GetValueOperand() const noexcept { return GetOperand(0); }
  const Value* GetPointer() const noexcept { return GetOperand(1); }
  uint64_t GetAlign() const noexcept { return align_; }

  static bool ClassOf(const Value* v) noexcept {
    const auto* i = v->As<Instruction>();
    return i && i->GetOpcode() == Opcode::kStore;
  }

 private:
  uint64_t align_;
};

/// \brief `getelementptr inbounds T, ptr P, idx...` — carries the source
///        element type (opaque-pointer style). Result type is `ptr`.
class GEPInst final : public Instruction {
 public:
  GEPInst(const Type* ptr_type, const Type* source_type, const Value* base,
          std::vector<const Value*> indices)
      : Instruction(Opcode::kGEP, ptr_type, {base}),
        source_type_(source_type) {
    for (const Value* idx : indices) AddOperand(idx);
  }

  const Type* GetSourceType() const noexcept { return source_type_; }
  const Value* GetBase() const noexcept { return GetOperand(0); }
  /// Indices are operands 1..N.
  unsigned GetNumIndices() const noexcept { return GetNumOperands() - 1; }
  const Value* GetIndex(unsigned i) const noexcept {
    return GetOperand(i + 1);
  }

  static bool ClassOf(const Value* v) noexcept {
    const auto* i = v->As<Instruction>();
    return i && i->GetOpcode() == Opcode::kGEP;
  }

 private:
  const Type* source_type_;
};

/// \brief All two-operand arithmetic/bitwise instructions (`add` .. `fdiv`).
///        Both operands and the result share one type.
class BinaryOperator final : public Instruction {
 public:
  BinaryOperator(Opcode opcode, const Type* type, const Value* lhs,
                 const Value* rhs)
      : Instruction(opcode, type, {lhs, rhs}) {}

  const Value* GetLHS() const noexcept { return GetOperand(0); }
  const Value* GetRHS() const noexcept { return GetOperand(1); }

  static bool ClassOf(const Value* v) noexcept {
    const auto* i = v->As<Instruction>();
    return i && i->IsBinaryOp();
  }
};

enum class CmpPredicate : uint8_t {
  // icmp
  kEQ, kNE, kSLT, kSLE, kSGT, kSGE, kULT, kULE, kUGT, kUGE,
  // fcmp (ordered except une, matching Clang's C lowering)
  kOEQ, kUNE, kOLT, kOLE, kOGT, kOGE,
};

/// The LLVM predicate keyword ("eq", "slt", "oeq", ...).
std::string_view GetPredicateName(CmpPredicate pred) noexcept;

/// \brief `icmp`/`fcmp`. Result type is i1.
class CmpInst final : public Instruction {
 public:
  CmpInst(Opcode opcode, const Type* i1_type, CmpPredicate pred,
          const Value* lhs, const Value* rhs)
      : Instruction(opcode, i1_type, {lhs, rhs}), pred_(pred) {}

  CmpPredicate GetPredicate() const noexcept { return pred_; }
  const Value* GetLHS() const noexcept { return GetOperand(0); }
  const Value* GetRHS() const noexcept { return GetOperand(1); }

  static bool ClassOf(const Value* v) noexcept {
    const auto* i = v->As<Instruction>();
    return i && (i->GetOpcode() == Opcode::kICmp ||
                 i->GetOpcode() == Opcode::kFCmp);
  }

 private:
  CmpPredicate pred_;
};

/// \brief All value casts (`trunc` .. `inttoptr`); the destination type is
///        the instruction's type.
class CastInst final : public Instruction {
 public:
  CastInst(Opcode opcode, const Type* dest_type, const Value* value)
      : Instruction(opcode, dest_type, {value}) {}

  const Value* GetOperandValue() const noexcept { return GetOperand(0); }

  static bool ClassOf(const Value* v) noexcept {
    const auto* i = v->As<Instruction>();
    return i && i->IsCast();
  }
};

/// \brief `br label %dest` (kBr) or `br i1 %c, label %t, label %f` (kCondBr).
class BranchInst final : public Instruction {
 public:
  /// Unconditional branch.
  BranchInst(const Type* void_type, BasicBlock* dest);
  /// Conditional branch.
  BranchInst(const Type* void_type, const Value* cond, BasicBlock* if_true,
             BasicBlock* if_false);

  bool IsConditional() const noexcept {
    return GetOpcode() == Opcode::kCondBr;
  }
  const Value* GetCondition() const noexcept { return GetOperand(0); }
  const BasicBlock* GetTrueDest() const noexcept;
  const BasicBlock* GetFalseDest() const noexcept;
  /// Unconditional target.
  const BasicBlock* GetDest() const noexcept;

  static bool ClassOf(const Value* v) noexcept {
    const auto* i = v->As<Instruction>();
    return i && (i->GetOpcode() == Opcode::kBr ||
                 i->GetOpcode() == Opcode::kCondBr);
  }
};

/// \brief `switch iN %v, label %default [ iN k, label %bb ... ]`.
class SwitchInst final : public Instruction {
 public:
  SwitchInst(const Type* void_type, const Value* cond, BasicBlock* dflt);

  void AddCase(const ConstantInt* value, BasicBlock* dest);
  /// The default is settable after creation: codegen discovers `default:`
  /// while walking the switch body (initially the epilog block).
  void SetDefaultDest(BasicBlock* dest);

  const Value* GetCondition() const noexcept { return GetOperand(0); }
  const BasicBlock* GetDefaultDest() const noexcept;
  unsigned GetNumCases() const noexcept { return GetNumOperands() / 2 - 1; }
  const ConstantInt* GetCaseValue(unsigned i) const noexcept;
  const BasicBlock* GetCaseDest(unsigned i) const noexcept;
};

/// \brief `ret void` or `ret T %v`.
class RetInst final : public Instruction {
 public:
  RetInst(const Type* void_type, const Value* value)
      : Instruction(Opcode::kRet, void_type,
                    value ? std::vector<const Value*>{value}
                          : std::vector<const Value*>{}) {}

  const Value* GetReturnValue() const noexcept {
    return GetNumOperands() ? GetOperand(0) : nullptr;
  }

  static bool ClassOf(const Value* v) noexcept {
    const auto* i = v->As<Instruction>();
    return i && i->GetOpcode() == Opcode::kRet;
  }
};

/// \brief `unreachable`.
class UnreachableInst final : public Instruction {
 public:
  explicit UnreachableInst(const Type* void_type)
      : Instruction(Opcode::kUnreachable, void_type, {}) {}

  static bool ClassOf(const Value* v) noexcept {
    const auto* i = v->As<Instruction>();
    return i && i->GetOpcode() == Opcode::kUnreachable;
  }
};

/// \brief `phi T [ %v, %bb ], ...` — used only for `?:`, `&&`, `||` values.
class PhiNode final : public Instruction {
 public:
  explicit PhiNode(const Type* type) : Instruction(Opcode::kPhi, type, {}) {}

  void AddIncoming(const Value* value, BasicBlock* block);

  unsigned GetNumIncoming() const noexcept { return GetNumOperands() / 2; }
  const Value* GetIncomingValue(unsigned i) const noexcept {
    return GetOperand(i * 2);
  }
  const BasicBlock* GetIncomingBlock(unsigned i) const noexcept;

  static bool ClassOf(const Value* v) noexcept {
    const auto* i = v->As<Instruction>();
    return i && i->GetOpcode() == Opcode::kPhi;
  }
};

/// \brief `call T %callee(args...)`. Carries the full function type so
///        variadic calls and function-pointer calls print correctly.
class CallInst final : public Instruction {
 public:
  CallInst(const FunctionType* fn_type, const Value* callee,
           std::vector<const Value*> args)
      : Instruction(Opcode::kCall, fn_type->GetReturnType(), {callee}),
        fn_type_(fn_type) {
    for (const Value* a : args) AddOperand(a);
  }

  const FunctionType* GetFunctionType() const noexcept { return fn_type_; }
  const Value* GetCallee() const noexcept { return GetOperand(0); }
  unsigned GetNumArgs() const noexcept { return GetNumOperands() - 1; }
  const Value* GetArg(unsigned i) const noexcept { return GetOperand(i + 1); }

  static bool ClassOf(const Value* v) noexcept {
    const auto* i = v->As<Instruction>();
    return i && i->GetOpcode() == Opcode::kCall;
  }

 private:
  const FunctionType* fn_type_;
};

}  // namespace bcc::ir
