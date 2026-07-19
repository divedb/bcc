#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bcc/ir/function.hh"
#include "bcc/ir/ir_context.hh"
#include "bcc/ir/value.hh"

namespace bcc::ir {

/// \brief One translation unit's worth of IR: global variables and
///        functions, plus the IRContext that owns all types and constants.
class Module {
 public:
  Module() = default;

  Module(const Module&) = delete;
  Module& operator=(const Module&) = delete;

  IRContext& GetContext() noexcept { return ctx_; }
  const IRContext& GetContext() const noexcept { return ctx_; }

  GlobalVariable* CreateGlobal(std::string name, const Type* value_type,
                               const Constant* initializer, Linkage linkage,
                               bool is_const, uint64_t align) {
    auto gv = std::make_unique<GlobalVariable>(
        ctx_.GetPointerType(), std::move(name), value_type, initializer,
        linkage, is_const, align);
    GlobalVariable* raw = gv.get();
    globals_.push_back(std::move(gv));
    return raw;
  }

  Function* CreateFunction(std::string name, const FunctionType* fn_type,
                           Linkage linkage) {
    auto f = std::make_unique<Function>(ctx_.GetPointerType(),
                                        std::move(name), fn_type, linkage);
    Function* raw = f.get();
    functions_.push_back(std::move(f));
    return raw;
  }

  /// First function with the given name, or null.
  Function* GetFunction(std::string_view name) const noexcept {
    for (const auto& f : functions_) {
      if (f->GetName() == name) return f.get();
    }
    return nullptr;
  }

  /// First global variable with the given name, or null.
  GlobalVariable* GetGlobal(std::string_view name) const noexcept {
    for (const auto& g : globals_) {
      if (g->GetName() == name) return g.get();
    }
    return nullptr;
  }

  const std::vector<std::unique_ptr<GlobalVariable>>& GetGlobals()
      const noexcept {
    return globals_;
  }
  const std::vector<std::unique_ptr<Function>>& GetFunctions()
      const noexcept {
    return functions_;
  }

 private:
  IRContext ctx_;
  std::vector<std::unique_ptr<GlobalVariable>> globals_;
  std::vector<std::unique_ptr<Function>> functions_;
};

}  // namespace bcc::ir
