#pragma once

#include <memory>
#include <string>
#include <vector>

#include "bcc/ir/basic_block.hh"
#include "bcc/ir/value.hh"

namespace bcc::ir {

/// \brief A function declaration or definition. An empty block list means a
///        declaration (printed as `declare`). As a Value it is the function's
///        address (type `ptr`). Owns its arguments and blocks.
class Function final : public GlobalValue {
 public:
  Function(const Type* ptr_type, std::string name,
           const FunctionType* fn_type, Linkage linkage)
      : GlobalValue(ValueKind::kFunction, ptr_type, std::move(name), linkage),
        fn_type_(fn_type) {
    const auto& params = fn_type->GetParams();
    args_.reserve(params.size());
    for (unsigned i = 0; i < params.size(); ++i) {
      args_.push_back(std::make_unique<Argument>(params[i], this, i));
    }
  }

  const FunctionType* GetFunctionType() const noexcept { return fn_type_; }
  const Type* GetReturnType() const noexcept {
    return fn_type_->GetReturnType();
  }

  unsigned GetNumArgs() const noexcept {
    return static_cast<unsigned>(args_.size());
  }
  Argument* GetArg(unsigned i) const noexcept { return args_[i].get(); }

  bool IsDeclaration() const noexcept { return blocks_.empty(); }

  BasicBlock* AppendBlock(std::unique_ptr<BasicBlock> bb) {
    BasicBlock* raw = bb.get();
    raw->SetParent(this);
    blocks_.push_back(std::move(bb));
    return raw;
  }

  const std::vector<std::unique_ptr<BasicBlock>>& GetBlocks() const noexcept {
    return blocks_;
  }
  BasicBlock* GetEntryBlock() const noexcept {
    return blocks_.empty() ? nullptr : blocks_.front().get();
  }

  /// Whether any call/initializer references this declaration (declarations
  /// are only printed when used, like Clang's deferred decls).
  bool IsUsed() const noexcept { return is_used_; }
  void SetUsed() noexcept { is_used_ = true; }

  static bool ClassOf(const Value* v) noexcept {
    return v->GetValueKind() == ValueKind::kFunction;
  }

 private:
  const FunctionType* fn_type_;
  std::vector<std::unique_ptr<Argument>> args_;
  std::vector<std::unique_ptr<BasicBlock>> blocks_;
  bool is_used_ = false;
};

}  // namespace bcc::ir
