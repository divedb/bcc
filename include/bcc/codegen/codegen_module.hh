#pragma once

#include <map>
#include <string>
#include <string_view>

#include "bcc/ast/ast_context.hh"
#include "bcc/ast/decl.hh"
#include "bcc/ast/expr.hh"
#include "bcc/basic/diagnostic.hh"
#include "bcc/codegen/codegen_types.hh"
#include "bcc/ir/module.hh"

namespace bcc::codegen {

/// \brief Per-translation-unit codegen state: walks the top-level decls,
///        creates and uniques IR globals/functions, uniques string literals,
///        and owns the type converter (Clang's CodeGenModule).
class CodeGenModule {
 public:
  CodeGenModule(ASTContext& ast_ctx, DiagnosticsEngine& diags,
                ir::Module& module)
      : ast_(ast_ctx), diags_(diags), module_(module),
        types_(ast_ctx, module.GetContext()) {}

  /// Lowers the whole TU: globals in two passes (create, then initialize,
  /// so forward references in initializers resolve), then function bodies.
  void EmitTranslationUnit();

  ASTContext& GetASTContext() noexcept { return ast_; }
  DiagnosticsEngine& GetDiags() noexcept { return diags_; }
  ir::Module& GetModule() noexcept { return module_; }
  ir::IRContext& GetIRContext() noexcept { return module_.GetContext(); }
  CodeGenTypes& GetTypes() noexcept { return types_; }

  /// The IR function for \p fd, created (as a declaration) on first use.
  ir::Function* GetOrCreateFunction(const FunctionDecl* fd);

  /// The IR global for a file-scope (or extern block-scope) \p vd.
  ir::GlobalVariable* GetOrCreateGlobal(const VarDecl* vd);

  /// The uniqued private constant for a string literal.
  ir::GlobalVariable* GetStringLiteral(const StringLiteral* s);

  /// An internal global for a static local (named "func.var", Clang's
  /// scheme) or a file-scope compound literal.
  ir::GlobalVariable* CreateStaticLocal(const VarDecl* vd,
                                        std::string_view func_name);
  ir::GlobalVariable* GetStaticLocal(const VarDecl* vd) const noexcept {
    auto it = static_locals_.find(vd);
    return it == static_locals_.end() ? nullptr : it->second;
  }

  /// Folds \p init (already Sema-checked as a constant initializer) to an
  /// IR constant of the lowered \p type; null if this emitter can't yet.
  const ir::Constant* EmitConstantInit(const Expr* init, QualType type);

  /// The all-zero constant of the lowered \p type.
  const ir::Constant* EmitNullConstant(QualType type);

  /// Reports err_codegen_cannot_compile for \p what at \p loc.
  void ErrorUnsupported(SourceLocation loc, std::string_view what);

 private:
  void EmitGlobalVarDefinition(const VarDecl* vd);
  void EmitFunctionDefinition(const FunctionDecl* fd);

  ASTContext& ast_;
  DiagnosticsEngine& diags_;
  ir::Module& module_;
  CodeGenTypes types_;

  std::map<std::string, ir::Function*, std::less<>> functions_;
  std::map<std::string, ir::GlobalVariable*, std::less<>> globals_;
  std::map<const VarDecl*, ir::GlobalVariable*> static_locals_;
  std::map<std::string, ir::GlobalVariable*> string_literals_;
  unsigned num_strings_ = 0;
};

}  // namespace bcc::codegen
