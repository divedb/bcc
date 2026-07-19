#include "bcc/sema/sema.hh"

#include "bcc/pp/preprocessor.hh"

namespace bcc {

Sema::Sema(Preprocessor& pp, ASTContext& ctx)
    : pp_(pp), ctx_(ctx), diags_(pp.GetDiagnostics()) {}

Sema::~Sema() = default;

//===----------------------------------------------------------------------===//
// Scope and name binding management.
//===----------------------------------------------------------------------===//

void Sema::PushOrdinaryDecl(Scope* s, NamedDecl* d) {
  if (const IdentifierInfo* name = d->GetIdentifier()) {
    ordinary_names_[name].push_back({s, d});
    s->AddOrdinaryDecl(d);
  }
}

void Sema::PushTagDecl(Scope* s, TagDecl* d) {
  if (const IdentifierInfo* name = d->GetIdentifier()) {
    tag_names_[name].push_back({s, d});
    s->AddTagDecl(d);
  }
}

void Sema::PopScope(Scope* s) {
  // Remove this scope's bindings from the lookup chains. A redeclaration in
  // the same scope replaces its previous binding in place, so each decl
  // removes at most one entry.
  for (NamedDecl* d : s->GetOrdinaryDecls()) {
    auto it = ordinary_names_.find(d->GetIdentifier());
    if (it == ordinary_names_.end()) continue;
    auto& chain = it->second;
    for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
      if (rit->first == s && rit->second == d) {
        chain.erase(std::next(rit).base());
        break;
      }
    }
  }
  for (TagDecl* d : s->GetTagDecls()) {
    auto it = tag_names_.find(d->GetIdentifier());
    if (it == tag_names_.end()) continue;
    auto& chain = it->second;
    for (auto rit = chain.rbegin(); rit != chain.rend(); ++rit) {
      if (rit->first == s && rit->second == d) {
        chain.erase(std::next(rit).base());
        break;
      }
    }
  }
  cur_scope_ = s->GetParent();
}

//===----------------------------------------------------------------------===//
// Name lookup.
//===----------------------------------------------------------------------===//

NamedDecl* Sema::LookupOrdinaryName(const IdentifierInfo* name) const {
  auto it = ordinary_names_.find(name);
  if (it == ordinary_names_.end() || it->second.empty()) return nullptr;
  return it->second.back().second;
}

TagDecl* Sema::LookupTagName(const IdentifierInfo* name) const {
  auto it = tag_names_.find(name);
  if (it == tag_names_.end() || it->second.empty()) return nullptr;
  return it->second.back().second;
}

NamedDecl* Sema::LookupOrdinaryNameInScope(const IdentifierInfo* name,
                                           Scope* s) const {
  auto it = ordinary_names_.find(name);
  if (it == ordinary_names_.end()) return nullptr;
  for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
    if (rit->first == s) return rit->second;
  }
  return nullptr;
}

TagDecl* Sema::LookupTagNameInScope(const IdentifierInfo* name,
                                    Scope* s) const {
  auto it = tag_names_.find(name);
  if (it == tag_names_.end()) return nullptr;
  for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
    if (rit->first == s) return rit->second;
  }
  return nullptr;
}

bool Sema::IsTypeName(const IdentifierInfo* name) const {
  NamedDecl* d = LookupOrdinaryName(name);
  return d && d->GetKind() == DeclKind::kTypedef;
}

//===----------------------------------------------------------------------===//
// End of translation unit.
//===----------------------------------------------------------------------===//

void Sema::ActOnEndOfTranslationUnit() {
  // C11 6.9.2p2: a tentative definition whose type is still incomplete at the
  // end of the translation unit behaves as if it had an initializer of 0 —
  // which requires a complete (or completable) type.
  for (VarDecl* vd : tentative_definitions_) {
    QualType t = vd->GetType();
    if (const auto* iat =
            t.GetCanonical().GetTypePtr()->As<IncompleteArrayType>()) {
      // `int x[];` becomes `int x[1];` at end of TU (C11 6.9.2p5).
      vd->SetType(ctx_.GetConstantArrayType(iat->GetElementType(), 1));
      continue;
    }
    if (!t->IsCompleteType()) {
      Diag(vd->GetLocation(), diag::err_typecheck_decl_incomplete_type)
          << t.GetAsString();
    }
  }
}

}  // namespace bcc
