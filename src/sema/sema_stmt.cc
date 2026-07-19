#include "bcc/pp/identifier_table.hh"
#include "bcc/sema/sema.hh"

namespace bcc {

StmtResult Sema::ActOnNullStmt(SourceLocation semi_loc) {
  return ctx_.New<NullStmt>(semi_loc);
}

StmtResult Sema::ActOnCompoundStmt(std::vector<Stmt*> body,
                                   SourceRange braces) {
  return ctx_.New<CompoundStmt>(std::move(body), braces);
}

StmtResult Sema::ActOnDeclStmt(std::vector<Decl*> decls, SourceRange range) {
  return ctx_.New<DeclStmt>(std::move(decls), range);
}

StmtResult Sema::ActOnExprStmt(Expr* e) {
  if (!e) return StmtError();
  return e;
}

StmtResult Sema::ActOnIfStmt(SourceLocation if_loc, Expr* cond,
                             Stmt* then_stmt, Stmt* else_stmt) {
  ExprResult checked = CheckBooleanCondition(cond, if_loc);
  if (checked.IsInvalid() || !then_stmt) return StmtError();
  SourceLocation end =
      else_stmt ? else_stmt->GetEndLoc() : then_stmt->GetEndLoc();
  return ctx_.New<IfStmt>(if_loc, checked.Get(), then_stmt, else_stmt, end);
}

StmtResult Sema::ActOnWhileStmt(SourceLocation while_loc, Expr* cond,
                                Stmt* body) {
  ExprResult checked = CheckBooleanCondition(cond, while_loc);
  if (checked.IsInvalid() || !body) return StmtError();
  return ctx_.New<WhileStmt>(while_loc, checked.Get(), body,
                             body->GetEndLoc());
}

StmtResult Sema::ActOnDoStmt(SourceLocation do_loc, Stmt* body, Expr* cond,
                             SourceLocation rparen) {
  ExprResult checked = CheckBooleanCondition(cond, do_loc);
  if (checked.IsInvalid() || !body) return StmtError();
  return ctx_.New<DoStmt>(do_loc, body, checked.Get(), rparen);
}

StmtResult Sema::ActOnForStmt(SourceLocation for_loc, Stmt* init, Expr* cond,
                              Expr* inc, Stmt* body) {
  Expr* checked_cond = nullptr;
  if (cond) {
    ExprResult checked = CheckBooleanCondition(cond, for_loc);
    if (checked.IsInvalid()) return StmtError();
    checked_cond = checked.Get();
  }
  if (!body) return StmtError();
  return ctx_.New<ForStmt>(for_loc, init, checked_cond, inc, body,
                           body->GetEndLoc());
}

//===----------------------------------------------------------------------===//
// switch / case / default (Clang: SemaStmt.cpp ActOnStartOfSwitchStmt...).
//===----------------------------------------------------------------------===//

StmtResult Sema::ActOnStartOfSwitchStmt(SourceLocation switch_loc,
                                        Expr* cond) {
  if (!cond) return StmtError();

  ExprResult conv = UsualUnaryConversions(cond);
  if (conv.IsInvalid()) return StmtError();
  cond = conv.Get();

  QualType t = cond->GetType();
  if (!t->IsIntegerType()) {
    Diag(switch_loc, diag::err_typecheck_statement_requires_integer)
        << t.GetAsString();
    return StmtError();
  }

  auto* sw = ctx_.New<SwitchStmt>(switch_loc, cond, nullptr, switch_loc);
  switch_stack_.push_back({sw, t, {}, nullptr});
  return sw;
}

StmtResult Sema::ActOnFinishSwitchStmt(Stmt* switch_stmt, Stmt* body) {
  if (!switch_stack_.empty() &&
      (!switch_stmt || switch_stack_.back().stmt == switch_stmt)) {
    switch_stack_.pop_back();
  }
  auto* sw = switch_stmt ? switch_stmt->As<SwitchStmt>() : nullptr;
  if (!sw || !body) return StmtError();
  sw->SetBody(body);
  return sw;
}

StmtResult Sema::ActOnCaseStmt(SourceLocation case_loc, Expr* value,
                               SourceLocation colon_loc) {
  (void)colon_loc;
  if (switch_stack_.empty()) {
    Diag(case_loc, diag::err_case_not_in_switch) << "case";
    return StmtError();
  }
  std::optional<ICEValue> v =
      VerifyICE(value, case_loc, diag::err_case_label_not_ice);
  if (!v) return StmtError();

  SwitchInfo& info = switch_stack_.back();

  // Convert the case value to the promoted switch-condition type.
  uint64_t width = ctx_.GetIntWidth(info.cond_type);
  int64_t converted = v->value;
  if (width < 64) {
    uint64_t mask = (uint64_t{1} << width) - 1;
    uint64_t uv = static_cast<uint64_t>(converted) & mask;
    if (!ctx_.IsUnsignedIntegerType(info.cond_type) &&
        (uv & (uint64_t{1} << (width - 1)))) {
      uv |= ~mask;
    }
    converted = static_cast<int64_t>(uv);
  }

  auto* cs = ctx_.New<CaseStmt>(case_loc, value, converted, nullptr);
  auto [it, inserted] = info.case_values.try_emplace(converted, cs);
  if (!inserted) {
    Diag(case_loc, diag::err_duplicate_case) << std::to_string(converted);
    Diag(it->second->GetBeginLoc(), diag::note_duplicate_case_prev);
    return StmtError();
  }
  info.stmt->AddCase(cs);
  return cs;
}

void Sema::ActOnCaseStmtBody(Stmt* case_stmt, Stmt* sub) {
  if (auto* cs = case_stmt ? case_stmt->As<CaseStmt>() : nullptr) {
    cs->SetSubStmt(sub);
  }
}

StmtResult Sema::ActOnDefaultStmt(SourceLocation default_loc, Stmt* sub) {
  if (switch_stack_.empty()) {
    Diag(default_loc, diag::err_case_not_in_switch) << "default";
    return StmtError();
  }
  SwitchInfo& info = switch_stack_.back();
  if (info.default_stmt) {
    Diag(default_loc, diag::err_multiple_default_labels_defined);
    Diag(info.default_stmt->GetBeginLoc(), diag::note_duplicate_default_prev);
    return StmtError();
  }
  auto* ds = ctx_.New<DefaultStmt>(default_loc, sub);
  info.default_stmt = ds;
  info.stmt->SetDefault(ds);
  return ds;
}

//===----------------------------------------------------------------------===//
// Jumps.
//===----------------------------------------------------------------------===//

StmtResult Sema::ActOnBreakStmt(SourceLocation loc, Scope* s) {
  if (!s->GetBreakParent()) {
    Diag(loc, diag::err_break_not_in_loop_or_switch);
    return StmtError();
  }
  return ctx_.New<BreakStmt>(loc);
}

StmtResult Sema::ActOnContinueStmt(SourceLocation loc, Scope* s) {
  if (!s->GetContinueParent()) {
    Diag(loc, diag::err_continue_not_in_loop);
    return StmtError();
  }
  return ctx_.New<ContinueStmt>(loc);
}

StmtResult Sema::ActOnReturnStmt(SourceLocation loc, Expr* value) {
  QualType return_type =
      cur_function_ ? cur_function_->GetReturnType() : ctx_.IntTy();
  std::string_view fn_name =
      cur_function_ ? cur_function_->GetName() : std::string_view("function");

  if (return_type->IsVoidType()) {
    if (value) {
      Diag(loc, diag::err_return_void_function) << fn_name;
      // Keep the expression in the AST for tooling; it is already checked.
    }
    return ctx_.New<ReturnStmt>(loc, value);
  }

  if (!value) {
    Diag(loc, diag::warn_return_missing_expr) << fn_name;
    return ctx_.New<ReturnStmt>(loc, nullptr);
  }

  QualType value_type = value->GetType();
  AssignConvertType result =
      CheckSingleAssignmentConstraints(return_type, value, &value_type);
  DiagnoseAssignmentResult(result, loc, return_type, value_type, "returning");
  if (result == AssignConvertType::kIncompatible) return StmtError();
  return ctx_.New<ReturnStmt>(loc, value);
}

StmtResult Sema::ActOnGotoStmt(SourceLocation goto_loc,
                               const IdentifierInfo* label,
                               SourceLocation label_loc) {
  LabelDecl* ld = LookupOrCreateLabel(label, label_loc);
  gotos_.push_back({ld, goto_loc});
  return ctx_.New<GotoStmt>(goto_loc, ld);
}

StmtResult Sema::ActOnLabelStmt(SourceLocation label_loc,
                                const IdentifierInfo* label, Stmt* sub) {
  LabelDecl* ld = LookupOrCreateLabel(label, label_loc);
  if (ld->IsDefined()) {
    Diag(label_loc, diag::err_label_redefinition) << label->GetName();
    Diag(ld->GetDefinitionLoc(), diag::note_previous_definition);
    return sub ? StmtResult(sub) : StmtError();
  }
  ld->SetDefined(label_loc);
  if (!sub) return StmtError();
  return ctx_.New<LabelStmt>(label_loc, ld, sub);
}

}  // namespace bcc
