#include "bcc/ir/instruction.hh"

#include "bcc/ir/basic_block.hh"

namespace bcc::ir {

std::string_view GetOpcodeName(Opcode op) noexcept {
  switch (op) {
    case Opcode::kAlloca: return "alloca";
    case Opcode::kLoad: return "load";
    case Opcode::kStore: return "store";
    case Opcode::kGEP: return "getelementptr";
    case Opcode::kAdd: return "add";
    case Opcode::kSub: return "sub";
    case Opcode::kMul: return "mul";
    case Opcode::kSDiv: return "sdiv";
    case Opcode::kUDiv: return "udiv";
    case Opcode::kSRem: return "srem";
    case Opcode::kURem: return "urem";
    case Opcode::kShl: return "shl";
    case Opcode::kLShr: return "lshr";
    case Opcode::kAShr: return "ashr";
    case Opcode::kAnd: return "and";
    case Opcode::kOr: return "or";
    case Opcode::kXor: return "xor";
    case Opcode::kFAdd: return "fadd";
    case Opcode::kFSub: return "fsub";
    case Opcode::kFMul: return "fmul";
    case Opcode::kFDiv: return "fdiv";
    case Opcode::kICmp: return "icmp";
    case Opcode::kFCmp: return "fcmp";
    case Opcode::kTrunc: return "trunc";
    case Opcode::kZExt: return "zext";
    case Opcode::kSExt: return "sext";
    case Opcode::kFPTrunc: return "fptrunc";
    case Opcode::kFPExt: return "fpext";
    case Opcode::kFPToSI: return "fptosi";
    case Opcode::kFPToUI: return "fptoui";
    case Opcode::kSIToFP: return "sitofp";
    case Opcode::kUIToFP: return "uitofp";
    case Opcode::kPtrToInt: return "ptrtoint";
    case Opcode::kIntToPtr: return "inttoptr";
    case Opcode::kBr: return "br";
    case Opcode::kCondBr: return "br";
    case Opcode::kSwitch: return "switch";
    case Opcode::kRet: return "ret";
    case Opcode::kUnreachable: return "unreachable";
    case Opcode::kPhi: return "phi";
    case Opcode::kCall: return "call";
  }
  return "";
}

std::string_view GetPredicateName(CmpPredicate pred) noexcept {
  switch (pred) {
    case CmpPredicate::kEQ: return "eq";
    case CmpPredicate::kNE: return "ne";
    case CmpPredicate::kSLT: return "slt";
    case CmpPredicate::kSLE: return "sle";
    case CmpPredicate::kSGT: return "sgt";
    case CmpPredicate::kSGE: return "sge";
    case CmpPredicate::kULT: return "ult";
    case CmpPredicate::kULE: return "ule";
    case CmpPredicate::kUGT: return "ugt";
    case CmpPredicate::kUGE: return "uge";
    case CmpPredicate::kOEQ: return "oeq";
    case CmpPredicate::kUNE: return "une";
    case CmpPredicate::kOLT: return "olt";
    case CmpPredicate::kOLE: return "ole";
    case CmpPredicate::kOGT: return "ogt";
    case CmpPredicate::kOGE: return "oge";
  }
  return "";
}

BranchInst::BranchInst(const Type* void_type, BasicBlock* dest)
    : Instruction(Opcode::kBr, void_type, {dest}) {}

BranchInst::BranchInst(const Type* void_type, const Value* cond,
                       BasicBlock* if_true, BasicBlock* if_false)
    : Instruction(Opcode::kCondBr, void_type, {cond, if_true, if_false}) {}

const BasicBlock* BranchInst::GetTrueDest() const noexcept {
  return GetOperand(1)->As<BasicBlock>();
}

const BasicBlock* BranchInst::GetFalseDest() const noexcept {
  return GetOperand(2)->As<BasicBlock>();
}

const BasicBlock* BranchInst::GetDest() const noexcept {
  return GetOperand(0)->As<BasicBlock>();
}

SwitchInst::SwitchInst(const Type* void_type, const Value* cond,
                       BasicBlock* dflt)
    : Instruction(Opcode::kSwitch, void_type, {cond, dflt}) {}

void SwitchInst::AddCase(const ConstantInt* value, BasicBlock* dest) {
  AddOperand(value);
  AddOperand(dest);
}

void SwitchInst::SetDefaultDest(BasicBlock* dest) { SetOperand(1, dest); }

const BasicBlock* SwitchInst::GetDefaultDest() const noexcept {
  return GetOperand(1)->As<BasicBlock>();
}

const ConstantInt* SwitchInst::GetCaseValue(unsigned i) const noexcept {
  return GetOperand(2 + i * 2)->As<ConstantInt>();
}

const BasicBlock* SwitchInst::GetCaseDest(unsigned i) const noexcept {
  return GetOperand(3 + i * 2)->As<BasicBlock>();
}

void PhiNode::AddIncoming(const Value* value, BasicBlock* block) {
  AddOperand(value);
  AddOperand(block);
}

const BasicBlock* PhiNode::GetIncomingBlock(unsigned i) const noexcept {
  return GetOperand(i * 2 + 1)->As<BasicBlock>();
}

}  // namespace bcc::ir
