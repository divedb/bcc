#pragma once

#include <memory>
#include <vector>

#include "bcc/ir/instruction.hh"

namespace bcc::ir {

class Function;

/// \brief An ordered list of instructions ending (in well-formed IR) with a
///        single terminator. Owns its instructions. As a Value it is a
///        branch target; blocks have no meaningful type (GetType() is null).
class BasicBlock final : public Value {
 public:
  explicit BasicBlock(std::string name_hint)
      : Value(ValueKind::kBasicBlock, nullptr) {
    SetName(std::move(name_hint));
  }

  Function* GetParent() const noexcept { return parent_; }
  void SetParent(Function* f) noexcept { parent_ = f; }

  Instruction* Append(std::unique_ptr<Instruction> inst) {
    Instruction* raw = inst.get();
    raw->SetParent(this);
    insts_.push_back(std::move(inst));
    return raw;
  }

  /// Inserts before position \p index (used for the entry-block alloca
  /// insertion point).
  Instruction* Insert(size_t index, std::unique_ptr<Instruction> inst) {
    Instruction* raw = inst.get();
    raw->SetParent(this);
    insts_.insert(insts_.begin() + static_cast<ptrdiff_t>(index),
                  std::move(inst));
    return raw;
  }

  const std::vector<std::unique_ptr<Instruction>>& GetInstructions()
      const noexcept {
    return insts_;
  }
  bool IsEmpty() const noexcept { return insts_.empty(); }
  size_t Size() const noexcept { return insts_.size(); }

  /// The terminator, or null if the block is not yet terminated.
  const Instruction* GetTerminator() const noexcept {
    if (insts_.empty() || !insts_.back()->IsTerminator()) return nullptr;
    return insts_.back().get();
  }
  bool IsTerminated() const noexcept { return GetTerminator() != nullptr; }

  static bool ClassOf(const Value* v) noexcept {
    return v->GetValueKind() == ValueKind::kBasicBlock;
  }

 private:
  Function* parent_ = nullptr;
  std::vector<std::unique_ptr<Instruction>> insts_;
};

}  // namespace bcc::ir
