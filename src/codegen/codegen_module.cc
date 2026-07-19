#include "bcc/codegen/codegen_module.hh"

#include <cassert>

#include "bcc/codegen/codegen_function.hh"

namespace bcc::codegen {

void CodeGenModule::EmitTranslationUnit() {
  const auto& decls = ast_.GetTranslationUnit()->GetDecls();

  // Pass 1: create every file-scope variable so initializers can forward-
  // reference (`int *p = &x;` before x's defining line).
  for (const Decl* d : decls) {
    if (const auto* vd = d->As<VarDecl>()) GetOrCreateGlobal(vd);
  }

  // Pass 2: attach initializers (definitions, tentative definitions).
  for (const Decl* d : decls) {
    if (const auto* vd = d->As<VarDecl>()) EmitGlobalVarDefinition(vd);
  }

  // Pass 3: function bodies.
  for (const Decl* d : decls) {
    if (const auto* fd = d->As<FunctionDecl>()) {
      if (fd->IsDefined()) EmitFunctionDefinition(fd);
    }
  }
}

ir::Function* CodeGenModule::GetOrCreateFunction(const FunctionDecl* fd) {
  std::string name(fd->GetName());
  auto it = functions_.find(name);
  if (it != functions_.end()) return it->second;

  const ir::FunctionType* fn_type = types_.ConvertFunctionType(fd->GetType());
  ir::Linkage linkage = fd->GetStorageClass() == StorageClass::kStatic
                            ? ir::Linkage::kInternal
                            : ir::Linkage::kExternal;
  ir::Function* fn = module_.CreateFunction(name, fn_type, linkage);
  functions_.emplace(std::move(name), fn);
  return fn;
}

ir::GlobalVariable* CodeGenModule::GetOrCreateGlobal(const VarDecl* vd) {
  std::string name(vd->GetName());
  auto it = globals_.find(name);
  if (it != globals_.end()) return it->second;

  QualType t = vd->GetType();
  const ir::Type* value_type = types_.Convert(t);
  uint64_t align = t->IsCompleteType() ? ast_.GetTypeAlign(t) : 1;
  ir::Linkage linkage = vd->GetStorageClass() == StorageClass::kStatic
                            ? ir::Linkage::kInternal
                            : ir::Linkage::kExternal;
  ir::GlobalVariable* gv = module_.CreateGlobal(
      name, value_type, /*initializer=*/nullptr, linkage,
      t.GetCanonical().HasConst(), align);
  globals_.emplace(std::move(name), gv);
  return gv;
}

void CodeGenModule::EmitGlobalVarDefinition(const VarDecl* vd) {
  ir::GlobalVariable* gv = GetOrCreateGlobal(vd);

  if (vd->HasInit()) {
    const ir::Constant* init = EmitConstantInit(vd->GetInit(), vd->GetType());
    if (!init) {
      ErrorUnsupported(vd->GetLocation(), "global initializer");
      init = EmitNullConstant(vd->GetType());
    }
    gv->SetInitializer(init);
    return;
  }

  // `extern` without initializer stays a declaration; everything else is a
  // tentative definition and gets a zero initializer.
  if (vd->GetStorageClass() != StorageClass::kExtern &&
      gv->GetInitializer() == nullptr) {
    gv->SetInitializer(EmitNullConstant(vd->GetType()));
  }
}

ir::GlobalVariable* CodeGenModule::GetStringLiteral(const StringLiteral* s) {
  // Bytes plus one terminating NUL element (element = char_byte_width bytes).
  std::string bytes(s->GetBytes());
  bytes.append(s->GetCharByteWidth(), '\0');

  auto it = string_literals_.find(bytes);
  if (it != string_literals_.end()) return it->second;

  const ir::ConstantString* cs = GetIRContext().GetString(bytes);
  std::string name =
      num_strings_ == 0 ? ".str" : ".str." + std::to_string(num_strings_);
  ++num_strings_;

  ir::GlobalVariable* gv =
      module_.CreateGlobal(std::move(name), cs->GetType(), cs,
                           ir::Linkage::kPrivate, /*is_const=*/true,
                           /*align=*/s->GetCharByteWidth());
  gv->SetUnnamedAddr(true);
  string_literals_.emplace(std::move(bytes), gv);
  return gv;
}

ir::GlobalVariable* CodeGenModule::CreateStaticLocal(
    const VarDecl* vd, std::string_view func_name) {
  auto it = static_locals_.find(vd);
  if (it != static_locals_.end()) return it->second;

  // Clang's scheme: an internal global named "func.var" (de-duplicated for
  // shadowed static locals).
  std::string base = std::string(func_name) + "." + std::string(vd->GetName());
  std::string name = base;
  for (unsigned n = 1; globals_.contains(name); ++n) {
    name = base + "." + std::to_string(n);
  }

  QualType t = vd->GetType();
  const ir::Constant* init = nullptr;
  if (vd->HasInit()) {
    init = EmitConstantInit(vd->GetInit(), t);
    if (!init) {
      ErrorUnsupported(vd->GetLocation(), "static local initializer");
    }
  }
  if (!init) init = EmitNullConstant(t);

  ir::GlobalVariable* gv = module_.CreateGlobal(
      name, types_.Convert(t), init, ir::Linkage::kInternal,
      t.GetCanonical().HasConst(), ast_.GetTypeAlign(t));
  globals_.emplace(std::move(name), gv);
  static_locals_.emplace(vd, gv);
  return gv;
}

const ir::Constant* CodeGenModule::EmitNullConstant(QualType type) {
  ir::IRContext& ctx = GetIRContext();
  const Type* canon = type.GetCanonical().GetTypePtr();
  if (canon->IsPointerType()) return ctx.GetNullPtr();
  if (canon->IsFloatingType()) return ctx.GetFP(types_.Convert(type), 0.0);
  if (canon->IsIntegerType()) {
    return ctx.GetInt(types_.Convert(type)->As<ir::IntegerType>(), 0);
  }
  return ctx.GetAggregateZero(types_.Convert(type));
}

void CodeGenModule::ErrorUnsupported(SourceLocation loc,
                                     std::string_view what) {
  diags_.Report(loc, diag::err_codegen_cannot_compile) << what;
}

void CodeGenModule::EmitFunctionDefinition(const FunctionDecl* fd) {
  ir::Function* fn = GetOrCreateFunction(fd);
  CodeGenFunction cgf(*this);
  cgf.EmitFunction(fd, fn);
}

}  // namespace bcc::codegen
