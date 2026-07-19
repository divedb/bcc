#include <algorithm>

#include "bcc/pp/identifier_table.hh"
#include "bcc/sema/sema.hh"

namespace bcc {

namespace {

StorageClass ToStorageClass(DeclSpec::SCS scs) {
  switch (scs) {
    case DeclSpec::SCS::kExtern: return StorageClass::kExtern;
    case DeclSpec::SCS::kStatic: return StorageClass::kStatic;
    case DeclSpec::SCS::kAuto: return StorageClass::kAuto;
    case DeclSpec::SCS::kRegister: return StorageClass::kRegister;
    default: return StorageClass::kNone;
  }
}

}  // namespace

//===----------------------------------------------------------------------===//
// Declarators (Clang: SemaDecl.cpp ActOnDeclarator / HandleDeclarator).
//===----------------------------------------------------------------------===//

DeclResult Sema::ActOnDeclarator(Scope* s, Declarator& d) {
  QualType type = GetTypeForDeclarator(d);
  if (type.IsNull() || d.IsInvalid()) return DeclResult::MakeInvalid();

  if (!d.HasName()) {
    Diag(d.GetDeclSpec().GetBeginLoc(),
         diag::err_declaration_does_not_declare_anything);
    return DeclResult::MakeInvalid();
  }

  if (d.GetDeclSpec().GetStorageClass() == DeclSpec::SCS::kTypedef) {
    return ActOnTypedefDeclarator(s, d, type);
  }

  StorageClass sc = ToStorageClass(d.GetDeclSpec().GetStorageClass());
  if (type->IsFunctionType()) return ActOnFunctionDeclarator(s, d, type, sc);
  return ActOnVariableDeclarator(s, d, type, sc);
}

DeclResult Sema::ActOnTypedefDeclarator(Scope* s, Declarator& d,
                                        QualType type) {
  const IdentifierInfo* name = d.GetIdentifier();
  SourceLocation loc = d.GetIdentifierLoc();

  if (NamedDecl* prev = LookupOrdinaryNameInScope(name, s)) {
    // C11 6.7p3: a typedef may be redeclared with the same type.
    if (auto* prev_td = prev->As<TypedefDecl>()) {
      if (ctx_.IsCompatible(prev_td->GetType(), type)) return prev;
      Diag(loc, diag::err_redefinition_different_type)
          << name->GetName() << type.GetAsString()
          << prev_td->GetType().GetAsString();
    } else {
      Diag(loc, diag::err_redefinition_different_kind) << name->GetName();
    }
    Diag(prev->GetLocation(), diag::note_previous_definition);
    return DeclResult::MakeInvalid();
  }

  auto* td = ctx_.New<TypedefDecl>(loc, name, type);
  PushOrdinaryDecl(s, td);
  return td;
}

DeclResult Sema::ActOnFunctionDeclarator(Scope* s, Declarator& d,
                                         QualType type, StorageClass sc) {
  const IdentifierInfo* name = d.GetIdentifier();
  SourceLocation loc = d.GetIdentifierLoc();
  bool is_file_scope = s->GetParent() == nullptr;

  if (sc == StorageClass::kAuto || sc == StorageClass::kRegister) {
    Diag(d.GetDeclSpec().GetStorageClassLoc(), diag::err_typecheck_sclass_func);
    sc = StorageClass::kNone;
  }
  if (!is_file_scope && sc == StorageClass::kStatic) {
    Diag(d.GetDeclSpec().GetStorageClassLoc(), diag::err_static_block_func);
    sc = StorageClass::kNone;
  }

  FunctionDecl* prev_fn = nullptr;
  if (NamedDecl* prev = LookupOrdinaryName(name)) {
    // Only merge with a previous *file-scope-visible* function; block-scope
    // shadowing of a variable by a function is a different-kind error only
    // if the previous decl is in the same scope.
    if (auto* pf = prev->As<FunctionDecl>()) {
      prev_fn = pf;
      if (!ctx_.IsCompatible(pf->GetType(), type)) {
        Diag(loc, diag::err_conflicting_types) << name->GetName();
        Diag(pf->GetLocation(), diag::note_previous_declaration);
        return DeclResult::MakeInvalid();
      }
      // C11 6.2.2p7: static after non-static is an error; non-static after
      // static just keeps internal linkage.
      bool prev_static = pf->GetStorageClass() == StorageClass::kStatic;
      if (sc == StorageClass::kStatic && !prev_static) {
        Diag(loc, diag::err_static_non_static) << name->GetName();
        Diag(pf->GetLocation(), diag::note_previous_declaration);
      }
      if (prev_static) sc = StorageClass::kStatic;
      type = ctx_.GetCompositeType(pf->GetType(), type);
    } else if (LookupOrdinaryNameInScope(name, s) == prev) {
      Diag(loc, diag::err_redefinition_different_kind) << name->GetName();
      Diag(prev->GetLocation(), diag::note_previous_definition);
      return DeclResult::MakeInvalid();
    }
  }

  auto* fd = ctx_.New<FunctionDecl>(loc, name, type, sc,
                                    d.GetDeclSpec().IsInline(),
                                    d.GetDeclSpec().IsNoreturn());

  // Adopt the parameters declared in the prototype scope.
  if (!d.GetChunks().empty() &&
      d.GetChunks().front().kind == DeclaratorChunk::Kind::kFunction) {
    std::vector<ParmVarDecl*> params;
    for (const DeclaratorChunk::ParamInfo& p : d.GetChunks().front().params) {
      if (p.decl) params.push_back(p.decl);
    }
    fd->SetParams(std::move(params));
  }

  if (prev_fn && prev_fn->IsDefined()) fd->SetBody(prev_fn->GetBody());
  PushOrdinaryDecl(s, fd);
  return fd;
}

DeclResult Sema::ActOnVariableDeclarator(Scope* s, Declarator& d,
                                         QualType type, StorageClass sc) {
  const IdentifierInfo* name = d.GetIdentifier();
  SourceLocation loc = d.GetIdentifierLoc();
  bool is_file_scope = s->GetParent() == nullptr;

  if (is_file_scope &&
      (sc == StorageClass::kAuto || sc == StorageClass::kRegister)) {
    Diag(d.GetDeclSpec().GetStorageClassLoc(),
         diag::err_typecheck_sclass_fscope);
    sc = StorageClass::kNone;
  }

  if (type->IsVoidType()) {
    Diag(loc, diag::err_variable_incomplete_type) << type.GetAsString();
    return DeclResult::MakeInvalid();
  }

  if (NamedDecl* prev = LookupOrdinaryNameInScope(name, s)) {
    auto* pv = prev->As<VarDecl>();
    if (!pv) {
      Diag(loc, diag::err_redefinition_different_kind) << name->GetName();
      Diag(prev->GetLocation(), diag::note_previous_definition);
      return DeclResult::MakeInvalid();
    }
    // Block-scope redeclaration is only legal when both declarations have
    // linkage (extern); file scope allows redeclaration freely.
    if (!is_file_scope && sc != StorageClass::kExtern) {
      Diag(loc, diag::err_redefinition) << name->GetName();
      Diag(prev->GetLocation(), diag::note_previous_definition);
      return DeclResult::MakeInvalid();
    }
    if (!ctx_.IsCompatible(pv->GetType(), type)) {
      Diag(loc, diag::err_redefinition_different_type)
          << name->GetName() << type.GetAsString()
          << pv->GetType().GetAsString();
      Diag(prev->GetLocation(), diag::note_previous_definition);
      return DeclResult::MakeInvalid();
    }
    bool prev_static = pv->GetStorageClass() == StorageClass::kStatic;
    bool prev_extern_or_none = !prev_static;
    if (sc == StorageClass::kStatic && prev_extern_or_none) {
      Diag(loc, diag::err_static_non_static) << name->GetName();
      Diag(prev->GetLocation(), diag::note_previous_declaration);
    } else if (prev_static && sc != StorageClass::kExtern &&
               sc != StorageClass::kStatic) {
      Diag(loc, diag::err_non_static_static) << name->GetName();
      Diag(prev->GetLocation(), diag::note_previous_declaration);
    }
    // Merge into the previous declaration: adopt the composite type.
    pv->SetType(ctx_.GetCompositeType(pv->GetType(), type));
    return pv;
  }

  auto* vd = ctx_.New<VarDecl>(loc, name, type, sc, is_file_scope);
  PushOrdinaryDecl(s, vd);
  return vd;
}

//===----------------------------------------------------------------------===//
// Initializers on declarations.
//===----------------------------------------------------------------------===//

void Sema::AddInitializerToDecl(Decl* decl, Expr* init) {
  if (!decl || !init) return;

  auto* vd = decl->As<VarDecl>();
  if (!vd) {
    Diag(init->GetBeginLoc(),
         decl->GetKind() == DeclKind::kTypedef
             ? diag::err_typedef_cannot_have_initializer
             : diag::err_func_cannot_have_initializer);
    return;
  }

  if (vd->GetStorageClass() == StorageClass::kExtern) {
    if (vd->IsFileScope()) {
      Diag(init->GetBeginLoc(), diag::err_extern_has_initializer);
    } else {
      Diag(init->GetBeginLoc(), diag::err_block_extern_cant_init);
      return;
    }
  }

  if (vd->HasInit()) {
    Diag(vd->GetLocation(), diag::err_redefinition) << vd->GetName();
    Diag(vd->GetLocation(), diag::note_previous_definition);
    return;
  }

  QualType type = vd->GetType();
  ExprResult checked =
      CheckInitializer(type, init, vd->HasStaticStorage());
  if (checked.IsInvalid()) return;
  vd->SetType(type);  // may have been completed from the initializer
  vd->SetInit(checked.Get());
}

void Sema::FinalizeDeclaration(Decl* decl) {
  auto* vd = decl ? decl->As<VarDecl>() : nullptr;
  if (!vd || vd->GetKind() == DeclKind::kParmVar) return;

  QualType t = vd->GetType();

  if (vd->HasInit()) return;

  if (vd->IsFileScope()) {
    // Non-extern file-scope declarations without an initializer are
    // tentative definitions; their type must be completed by end of TU.
    if (vd->GetStorageClass() != StorageClass::kExtern &&
        !t->IsCompleteType()) {
      tentative_definitions_.push_back(vd);
    }
    return;
  }

  // Block scope: a non-extern object must have a complete type.
  if (vd->GetStorageClass() != StorageClass::kExtern &&
      !t->IsCompleteType()) {
    Diag(vd->GetLocation(), diag::err_variable_incomplete_type)
        << t.GetAsString();
  }
}

DeclResult Sema::ActOnEmptyDeclaration(Scope* s, const DeclSpec& ds,
                                       SourceLocation semi_loc) {
  (void)s;
  // `struct S;` / `enum E { ... };` declare the tag; anything else declares
  // nothing.
  if (TagDecl* tag = ds.GetTagDecl()) return tag;
  Diag(ds.GetBeginLoc().IsValid() ? ds.GetBeginLoc() : semi_loc,
       diag::err_declaration_does_not_declare_anything);
  return DeclResult();
}

//===----------------------------------------------------------------------===//
// Parameters.
//===----------------------------------------------------------------------===//

ParmVarDecl* Sema::ActOnParamDeclarator(Scope* s, Declarator& d) {
  QualType type = GetTypeForDeclarator(d);
  if (type.IsNull()) return nullptr;

  DeclSpec::SCS scs = d.GetDeclSpec().GetStorageClass();
  if (scs != DeclSpec::SCS::kUnspecified && scs != DeclSpec::SCS::kRegister) {
    Diag(d.GetDeclSpec().GetStorageClassLoc(),
         diag::err_invalid_storage_class_in_func_decl);
  }

  // C11 6.7.6.3p7-8: array and function parameter types adjust to pointers.
  type = ctx_.GetDecayedType(type);

  const IdentifierInfo* name = d.GetIdentifier();
  SourceLocation loc =
      d.HasName() ? d.GetIdentifierLoc() : d.GetDeclSpec().GetBeginLoc();

  if (name) {
    if (NamedDecl* prev = LookupOrdinaryNameInScope(name, s)) {
      Diag(loc, diag::err_redefinition) << name->GetName();
      Diag(prev->GetLocation(), diag::note_previous_definition);
      return nullptr;
    }
  }

  auto* pd = ctx_.New<ParmVarDecl>(loc, name, type);
  if (name) PushOrdinaryDecl(s, pd);
  return pd;
}

//===----------------------------------------------------------------------===//
// Function definitions.
//===----------------------------------------------------------------------===//

FunctionDecl* Sema::ActOnStartOfFunctionDef(Scope* fn_scope, Declarator& d) {
  // The declaration itself belongs to the scope enclosing the function body.
  Scope* decl_scope = fn_scope->GetParent();
  DeclResult result = ActOnDeclarator(decl_scope, d);
  FunctionDecl* fd =
      result.IsUsable() ? result.Get()->As<FunctionDecl>() : nullptr;
  if (!fd) return nullptr;

  if (fd->IsDefined()) {
    Diag(d.GetIdentifierLoc(), diag::err_redefinition) << fd->GetName();
    Diag(fd->GetLocation(), diag::note_previous_definition);
  }

  QualType ret = fd->GetReturnType();
  if (!ret->IsVoidType() && !ret->IsCompleteType()) {
    Diag(fd->GetLocation(), diag::err_func_def_incomplete_result)
        << ret.GetAsString();
  }

  // Inject the parameters into the body scope; they must all be named.
  for (ParmVarDecl* p : fd->GetParams()) {
    if (!p->GetIdentifier()) {
      Diag(p->GetLocation(), diag::err_parameter_name_omitted);
      continue;
    }
    if (!p->GetType()->IsCompleteType()) {
      Diag(p->GetLocation(), diag::err_param_incomplete_type)
          << p->GetType().GetAsString();
    }
    PushOrdinaryDecl(fn_scope, p);
  }

  cur_function_ = fd;
  function_labels_.clear();
  gotos_.clear();
  return fd;
}

void Sema::ActOnFinishFunctionBody(FunctionDecl* fd, Stmt* body) {
  if (fd && body) {
    fd->SetBody(body->As<CompoundStmt>());
  }
  for (auto& [label, goto_loc] : gotos_) {
    if (!label->IsDefined()) {
      Diag(goto_loc, diag::err_undeclared_label_use) << label->GetName();
    }
  }
  cur_function_ = nullptr;
  function_labels_.clear();
  gotos_.clear();
}

LabelDecl* Sema::LookupOrCreateLabel(const IdentifierInfo* name,
                                     SourceLocation loc) {
  auto [it, inserted] = function_labels_.try_emplace(name, nullptr);
  if (inserted) it->second = ctx_.New<LabelDecl>(loc, name);
  return it->second;
}

//===----------------------------------------------------------------------===//
// Tags (Clang: SemaDecl.cpp ActOnTag).
//===----------------------------------------------------------------------===//

TagDecl* Sema::ActOnTag(Scope* s, TagKind kind, TagUseKind use,
                        SourceLocation kw_loc, const IdentifierInfo* name,
                        SourceLocation name_loc) {
  auto create = [&](Scope* in_scope) -> TagDecl* {
    TagDecl* tag;
    if (kind == TagKind::kEnum) {
      tag = ctx_.New<EnumDecl>(name ? name_loc : kw_loc, name);
    } else {
      tag = ctx_.New<RecordDecl>(kind, name ? name_loc : kw_loc, name);
    }
    if (name) PushTagDecl(in_scope, tag);
    return tag;
  };

  if (!name) return create(s);

  TagDecl* prev = use == TagUseKind::kDeclaration
                      ? LookupTagNameInScope(name, s)
                      : LookupTagName(name);
  if (!prev) return create(s);

  if (prev->GetTagKind() != kind) {
    Diag(kw_loc, diag::err_use_with_wrong_tag) << name->GetName();
    Diag(prev->GetLocation(), diag::note_previous_definition);
    return create(s);
  }

  if (use != TagUseKind::kDefinition) return prev;

  // Definition: only a same-scope incomplete forward declaration may be
  // completed; a same-scope complete tag is a redefinition, and any
  // other-scope tag is shadowed by a fresh declaration.
  bool in_same_scope = LookupTagNameInScope(name, s) == prev;
  if (!in_same_scope) return create(s);

  if (std::find(tags_being_defined_.begin(), tags_being_defined_.end(),
                prev) != tags_being_defined_.end()) {
    Diag(kw_loc, diag::err_nested_redefinition) << name->GetName();
    Diag(prev->GetLocation(), diag::note_previous_definition);
    return create(s);
  }
  if (prev->IsCompleteDefinition()) {
    Diag(name_loc, diag::err_redefinition) << name->GetName();
    Diag(prev->GetLocation(), diag::note_previous_definition);
    return create(s);
  }
  return prev;
}

void Sema::ActOnTagStartDefinition(TagDecl* tag) {
  tags_being_defined_.push_back(tag);
}

void Sema::ActOnTagFinishDefinition(TagDecl* tag, SourceLocation rbrace_loc) {
  (void)rbrace_loc;
  tag->SetCompleteDefinition();
  tags_being_defined_.pop_back();

  if (auto* record = tag->As<RecordDecl>()) {
    const auto& fields = record->GetFields();
    for (std::size_t i = 0; i < fields.size(); ++i) {
      QualType ft = fields[i]->GetType();
      if (ft.GetCanonical().GetTypePtr()->As<IncompleteArrayType>()) {
        if (record->IsUnion() || i + 1 != fields.size()) {
          Diag(fields[i]->GetLocation(), diag::err_flexible_array_not_at_end);
        } else if (i == 0) {
          Diag(fields[i]->GetLocation(), diag::err_flexible_array_empty_struct)
              << fields[i]->GetName();
        } else {
          record->SetHasFlexibleArrayMember();
        }
      }
    }
  }
}

FieldDecl* Sema::ActOnField(Scope* s, RecordDecl* record, Declarator& d,
                            Expr* bitfield_width) {
  (void)s;
  QualType type = GetTypeForDeclarator(d);
  if (type.IsNull()) return nullptr;

  const IdentifierInfo* name = d.GetIdentifier();
  SourceLocation loc =
      d.HasName() ? d.GetIdentifierLoc() : d.GetDeclSpec().GetBeginLoc();

  if (d.GetDeclSpec().GetStorageClass() != DeclSpec::SCS::kUnspecified) {
    Diag(d.GetDeclSpec().GetStorageClassLoc(),
         diag::err_typename_invalid_storageclass);
  }

  if (type->IsFunctionType()) {
    Diag(loc, diag::err_field_function_type)
        << (name ? name->GetName() : std::string_view("<anonymous>"));
    return nullptr;
  }

  // Incomplete member types: allowed only as a flexible array member (the
  // position constraint is checked when the tag definition finishes).
  bool is_flexible_array =
      type.GetCanonical().GetTypePtr()->As<IncompleteArrayType>() != nullptr;
  if (!type->IsCompleteType() && !is_flexible_array) {
    Diag(loc, diag::err_field_incomplete) << type.GetAsString();
    return nullptr;
  }
  if (type.GetCanonical().GetTypePtr()->As<VariableArrayType>()) {
    Diag(loc, diag::err_typecheck_field_variable_size);
    return nullptr;
  }

  // Duplicate member check against the direct fields declared so far.
  if (name) {
    for (const FieldDecl* f : record->GetFields()) {
      if (f->GetIdentifier() == name) {
        Diag(loc, diag::err_duplicate_member) << name->GetName();
        Diag(f->GetLocation(), diag::note_previous_member);
        return nullptr;
      }
    }
  }

  unsigned width_value = 0;
  if (bitfield_width) {
    std::string_view field_name =
        name ? name->GetName() : std::string_view("<anonymous>");
    if (!type->IsIntegerType()) {
      Diag(loc, diag::err_bitfield_not_integer)
          << field_name << type.GetAsString();
      return nullptr;
    }
    std::optional<ICEValue> width = EvaluateICE(bitfield_width);
    if (!width) {
      Diag(loc, diag::err_bitfield_width_not_ice) << field_name;
      return nullptr;
    }
    if (!width->is_unsigned && width->value < 0) {
      Diag(loc, diag::err_bitfield_negative_width)
          << field_name << std::to_string(width->value);
      return nullptr;
    }
    uint64_t type_bits = ctx_.GetIntWidth(type);
    if (static_cast<uint64_t>(width->value) > type_bits) {
      Diag(loc, diag::err_bitfield_width_exceeds_type_width)
          << field_name << static_cast<unsigned long long>(width->value)
          << static_cast<unsigned long long>(type_bits);
      return nullptr;
    }
    if (width->value == 0 && name) {
      Diag(loc, diag::err_bitfield_zero_width_named) << field_name;
      return nullptr;
    }
    width_value = static_cast<unsigned>(width->value);
  }

  auto* field =
      ctx_.New<FieldDecl>(loc, name, type, bitfield_width, width_value);
  record->AddField(field);
  return field;
}

//===----------------------------------------------------------------------===//
// Enum constants.
//===----------------------------------------------------------------------===//

EnumConstantDecl* Sema::ActOnEnumConstant(Scope* s, EnumDecl* enum_decl,
                                          EnumConstantDecl* last,
                                          SourceLocation id_loc,
                                          const IdentifierInfo* name,
                                          Expr* value_expr) {
  if (NamedDecl* prev = LookupOrdinaryNameInScope(name, s)) {
    Diag(id_loc, diag::err_redefinition) << name->GetName();
    Diag(prev->GetLocation(), diag::note_previous_definition);
    return nullptr;
  }

  int64_t value = 0;
  if (value_expr) {
    std::optional<int64_t> v = CheckEnumConstantValue(value_expr, id_loc);
    if (!v) return nullptr;
    value = *v;
  } else if (last) {
    value = last->GetValue() + 1;
    if (value > INT32_MAX) {
      Diag(id_loc, diag::err_enumerator_too_large);
    }
  }

  // bcc fixes every enum's underlying/compatible type to int.
  auto* ec = ctx_.New<EnumConstantDecl>(id_loc, name, ctx_.IntTy(), value);
  enum_decl->AddEnumerator(ec);
  PushOrdinaryDecl(s, ec);
  return ec;
}

std::optional<int64_t> Sema::CheckEnumConstantValue(Expr* value_expr,
                                                    SourceLocation loc) {
  std::optional<ICEValue> v =
      VerifyICE(value_expr, loc, diag::err_enum_invalid_underlying);
  if (!v) return std::nullopt;
  if (v->value > INT32_MAX || v->value < INT32_MIN) {
    Diag(loc, diag::err_enumerator_too_large);
    return std::nullopt;
  }
  return v->value;
}

//===----------------------------------------------------------------------===//
// _Static_assert.
//===----------------------------------------------------------------------===//

DeclResult Sema::ActOnStaticAssert(SourceLocation loc, Expr* cond,
                                   std::string message) {
  std::optional<ICEValue> value =
      VerifyICE(cond, loc, diag::err_static_assert_expression_is_not_constant);
  if (value && value->value == 0) {
    std::string suffix;
    if (!message.empty()) suffix = ": \"" + message + "\"";
    Diag(loc, diag::err_static_assert_failed) << suffix;
  }
  auto* sa = ctx_.New<StaticAssertDecl>(loc, cond, std::move(message));
  return sa;
}

}  // namespace bcc
