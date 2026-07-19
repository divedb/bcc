#include <algorithm>

#include "bcc/pp/identifier_table.hh"
#include "bcc/sema/sema.hh"

namespace bcc {

namespace {

/// True if \p e (after implicit conversions) is a valid static-storage
/// constant initializer (C11 6.7.9p4: constant expressions or string
/// literals; address constants for pointers).
bool IsConstantInitializer(const Sema& sema, const Expr* e) {
  if (!e) return true;
  const Expr* inner = e->IgnoreParenImpCasts();

  // A constant value, possibly behind implicit conversions the evaluator
  // does not model (e.g. NullToPointer, IntegralToFloating).
  if (sema.EvaluateICE(e) || sema.EvaluateICE(inner)) return true;

  switch (inner->GetStmtClass()) {
    case StmtClass::kFloatingLiteral:
    case StmtClass::kStringLiteral:
    case StmtClass::kCompoundLiteralExpr:
      return true;
    case StmtClass::kInitListExpr: {
      for (const Expr* init :
           static_cast<const InitListExpr*>(inner)->GetInits()) {
        if (init && !IsConstantInitializer(sema, init)) return false;
      }
      return true;
    }
    case StmtClass::kDeclRefExpr: {
      // An array or function decays to an address constant.
      const auto* ref = static_cast<const DeclRefExpr*>(inner);
      if (ref->GetDecl()->GetKind() == DeclKind::kFunction) return true;
      if (const auto* vd = ref->GetDecl()->As<VarDecl>()) {
        return vd->HasStaticStorage() && vd->GetType()->IsArrayType();
      }
      return false;
    }
    case StmtClass::kUnaryOperator: {
      // &object with static storage duration is an address constant.
      const auto* uo = static_cast<const UnaryOperator*>(inner);
      if (uo->GetOpcode() != UnaryOperatorKind::kAddrOf) return false;
      const Expr* operand = uo->GetSubExpr()->IgnoreParens();
      if (const auto* ref = operand->As<DeclRefExpr>()) {
        if (ref->GetDecl()->GetKind() == DeclKind::kFunction) return true;
        if (const auto* vd = ref->GetDecl()->As<VarDecl>()) {
          return vd->HasStaticStorage();
        }
      }
      return false;
    }
    case StmtClass::kCStyleCastExpr:
      return IsConstantInitializer(
          sema, static_cast<const CStyleCastExpr*>(inner)->GetSubExpr());
    case StmtClass::kBinaryOperator: {
      // Address constant plus/minus an integer constant (C11 6.6p7).
      const auto* bo = static_cast<const BinaryOperator*>(inner);
      if (bo->GetOpcode() != BinaryOperatorKind::kAdd &&
          bo->GetOpcode() != BinaryOperatorKind::kSub) {
        return false;
      }
      if (!bo->GetType()->IsPointerType()) return false;
      const Expr* pointer =
          bo->GetLHS()->GetType()->IsPointerType() ? bo->GetLHS()
                                                   : bo->GetRHS();
      const Expr* index = pointer == bo->GetLHS() ? bo->GetRHS()
                                                  : bo->GetLHS();
      return IsConstantInitializer(sema, pointer) &&
             sema.EvaluateICE(index).has_value();
    }
    default:
      return false;
  }
}

/// True if a string literal of kind \p sl_type can initialize an array whose
/// element type is \p elem (C11 6.7.9p14-15).
bool StringLiteralFitsArray(ASTContext& ctx, const StringLiteral* sl,
                            QualType elem) {
  elem = elem.GetCanonical().WithoutQualifiers();
  const auto* bt = elem.GetTypePtr()->As<BuiltinType>();
  if (!bt) return false;
  switch (bt->GetKind()) {
    case BuiltinTypeKind::kChar:
    case BuiltinTypeKind::kSChar:
    case BuiltinTypeKind::kUChar:
      return sl->GetCharByteWidth() == 1;
    default:
      return ctx.GetTypeSize(elem) == sl->GetCharByteWidth();
  }
}

/// \brief Walks a syntactic initializer list, producing the semantic form:
///        one initializer per subobject in layout order (Clang's
///        InitListChecker, reduced to C essentials).
class InitListChecker {
 public:
  InitListChecker(Sema& sema, bool is_static)
      : sema_(sema), ctx_(sema.GetASTContext()), is_static_(is_static) {}

  bool HadError() const noexcept { return had_error_; }

  /// Checks the braced list \p ilist against \p type. Completes incomplete
  /// array types. Returns the semantic-form list.
  Expr* CheckExplicitInitList(QualType& type, InitListExpr* ilist);

 private:
  struct Cursor {
    const std::vector<const Expr*>& items;
    std::size_t pos = 0;

    bool AtEnd() const noexcept { return pos >= items.size(); }
    const Expr* Peek() const noexcept { return items[pos]; }
  };

  /// Initializes one subobject of type \p type from the cursor: consumes a
  /// braced sublist, a string literal (for char arrays), a scalar item, or
  /// descends implicitly into an aggregate subobject.
  Expr* CheckSubobject(QualType type, Cursor& cur);

  Expr* CheckArray(QualType& type, Cursor& cur, bool outer_braces);
  Expr* CheckRecord(QualType type, Cursor& cur);
  Expr* CheckScalar(QualType type, Cursor& cur);

  /// Applies designators [d .. end) to reach a subobject of \p type, then
  /// initializes it with \p init. Used for `.a[1].b = x`.
  Expr* CheckDesignatedSubobject(QualType type,
                                 const std::vector<Designator>& designators,
                                 std::size_t d, const Expr* init);

  Expr* CheckLeaf(QualType type, const Expr* init);

  Sema& sema_;
  ASTContext& ctx_;
  bool is_static_;
  bool had_error_ = false;
};

Expr* InitListChecker::CheckLeaf(QualType type, const Expr* init) {
  Expr* e = const_cast<Expr*>(init);

  // A nested braced list initializing a nested aggregate.
  if (auto* ilist = e->As<InitListExpr>()) {
    QualType t = type;
    Expr* result = CheckExplicitInitList(t, ilist);
    return result;
  }

  QualType canon = type.GetCanonical();
  if (canon.GetTypePtr()->IsArrayType()) {
    // Only a string literal can initialize an array from a single expression.
    const Expr* inner = e->IgnoreParens();
    if (const auto* sl = inner->As<StringLiteral>()) {
      if (StringLiteralFitsArray(ctx_, sl, canon.GetTypePtr()
                                               ->GetArrayElementType())) {
        return const_cast<Expr*>(inner);
      }
    }
    sema_.Diag(e->GetBeginLoc(), diag::err_array_init_not_init_list);
    had_error_ = true;
    return e;
  }

  QualType init_type = init->GetType();
  Sema::AssignConvertType result =
      sema_.CheckSingleAssignmentConstraints(type, e, &init_type);
  sema_.DiagnoseAssignmentResult(result, e->GetBeginLoc(), type, init_type,
                                 "initializing");
  if (result == Sema::AssignConvertType::kIncompatible) had_error_ = true;

  if (is_static_ && !IsConstantInitializer(sema_, e)) {
    sema_.Diag(e->GetBeginLoc(), diag::err_init_element_not_constant);
    had_error_ = true;
  }
  return e;
}

Expr* InitListChecker::CheckSubobject(QualType type, Cursor& cur) {
  if (cur.AtEnd()) return nullptr;

  const Expr* item = cur.Peek();

  // Explicit braces or a plain expression that matches directly.
  QualType canon = type.GetCanonical();
  bool is_aggregate = canon.GetTypePtr()->IsArrayType() ||
                      canon.GetTypePtr()->IsRecordType();

  if (item->GetStmtClass() == StmtClass::kInitListExpr) {
    ++cur.pos;
    return CheckLeaf(type, item);
  }

  if (!is_aggregate) {
    ++cur.pos;
    return CheckLeaf(type, item);
  }

  // Aggregate subobject initialized from a bare expression list: either the
  // expression initializes the whole subobject (compatible struct or string
  // literal), or we descend implicitly and let the subobject consume items.
  const Expr* inner = item->IgnoreParens();
  if (canon.GetTypePtr()->IsRecordType() &&
      ctx_.IsCompatible(canon.WithoutQualifiers(),
                        inner->GetType().GetCanonical().WithoutQualifiers())) {
    ++cur.pos;
    return CheckLeaf(type, item);
  }
  if (canon.GetTypePtr()->IsArrayType() && inner->As<StringLiteral>()) {
    ++cur.pos;
    return CheckLeaf(type, item);
  }

  // Implicit descent (C11 6.7.9p20): the subobject consumes as many items
  // as it needs from the current cursor.
  QualType t = type;
  if (canon.GetTypePtr()->IsArrayType()) {
    return CheckArray(t, cur, /*outer_braces=*/false);
  }
  return CheckRecord(t, cur);
}

Expr* InitListChecker::CheckDesignatedSubobject(
    QualType type, const std::vector<Designator>& designators, std::size_t d,
    const Expr* init) {
  if (d >= designators.size()) return CheckLeaf(type, init);

  // Build a one-element semantic list around the designated subobject.
  const Designator& des = designators[d];
  QualType canon = type.GetCanonical();

  if (des.IsField()) {
    const auto* rt = canon.GetTypePtr()->As<RecordType>();
    if (!rt) {
      sema_.Diag(des.loc, diag::err_field_designator_non_aggr)
          << type.GetAsString();
      had_error_ = true;
      return nullptr;
    }
    std::vector<const FieldDecl*> path;
    const FieldDecl* field = rt->GetDecl()->FindField(des.field, &path);
    if (!field) {
      sema_.Diag(des.loc, diag::err_field_designator_unknown)
          << des.field->GetName() << type.GetAsString();
      had_error_ = true;
      return nullptr;
    }
    const std::vector<FieldDecl*>& fields = rt->GetDecl()->GetFields();
    std::size_t index = std::distance(
        fields.begin(), std::find(fields.begin(), fields.end(), path[0]));

    Expr* sub =
        CheckDesignatedSubobject(path[0]->GetType(), designators, d + 1, init);

    std::vector<const Expr*> inits;
    if (rt->GetDecl()->IsUnion()) {
      inits.push_back(sub);
    } else {
      inits.resize(fields.size(), nullptr);
      inits[index] = sub;
    }
    auto* semantic =
        ctx_.New<InitListExpr>(std::move(inits), init->GetSourceRange());
    semantic->SetType(type);
    if (rt->GetDecl()->IsUnion()) semantic->SetInitializedField(path[0]);
    return semantic;
  }

  const auto* at = canon.GetTypePtr()->As<ArrayType>();
  if (!at) {
    sema_.Diag(des.loc, diag::err_array_designator_non_array)
        << type.GetAsString();
    had_error_ = true;
    return nullptr;
  }
  std::optional<ICEValue> index = sema_.VerifyICE(
      des.index, des.loc, diag::err_array_designator_not_ice);
  if (!index) {
    had_error_ = true;
    return nullptr;
  }
  Expr* sub =
      CheckDesignatedSubobject(at->GetElementType(), designators, d + 1, init);
  std::vector<const Expr*> inits(static_cast<std::size_t>(index->value) + 1,
                                 nullptr);
  inits.back() = sub;
  auto* semantic =
      ctx_.New<InitListExpr>(std::move(inits), init->GetSourceRange());
  semantic->SetType(type);
  return semantic;
}

Expr* InitListChecker::CheckArray(QualType& type, Cursor& cur,
                                  bool outer_braces) {
  QualType canon = type.GetCanonical();
  const auto* at = canon.GetTypePtr()->As<ArrayType>();
  QualType elem = at->GetElementType();

  std::optional<uint64_t> bound;
  if (const auto* cat = at->As<ConstantArrayType>()) bound = cat->GetSize();

  std::vector<const Expr*> semantic;
  uint64_t index = 0;
  uint64_t max_index_seen = 0;
  bool any = false;

  auto place = [&](uint64_t at_index, Expr* e) {
    if (semantic.size() <= at_index) semantic.resize(at_index + 1, nullptr);
    if (semantic[at_index]) {
      sema_.Diag(e ? e->GetBeginLoc() : SourceLocation{},
                 diag::err_initializer_overrides);
    }
    semantic[at_index] = e;
  };

  while (!cur.AtEnd()) {
    const Expr* item = cur.Peek();

    if (const auto* de = item->As<DesignatedInitExpr>()) {
      const Designator& first = de->GetDesignators().front();
      if (first.IsField()) {
        // A field designator ends this array's items when descending
        // implicitly; at the top brace level it is an error.
        if (!outer_braces) break;
        sema_.Diag(first.loc, diag::err_array_designator_non_array)
            << type.GetAsString();
        had_error_ = true;
        ++cur.pos;
        continue;
      }
      std::optional<ICEValue> di = sema_.VerifyICE(
          first.index, first.loc, diag::err_array_designator_not_ice);
      ++cur.pos;
      if (!di) {
        had_error_ = true;
        continue;
      }
      if (!di->is_unsigned && di->value < 0) {
        sema_.Diag(first.loc, diag::err_array_designator_negative)
            << std::to_string(di->value);
        had_error_ = true;
        continue;
      }
      index = static_cast<uint64_t>(di->value);
      if (bound && index >= *bound) {
        sema_.Diag(first.loc, diag::err_array_designator_too_large)
            << std::to_string(index) << std::to_string(*bound);
        had_error_ = true;
        continue;
      }
      Expr* sub;
      if (de->GetDesignators().size() > 1) {
        sub = CheckDesignatedSubobject(elem, de->GetDesignators(), 1,
                                       de->GetInit());
      } else {
        sub = CheckLeaf(elem, de->GetInit());
      }
      place(index, sub);
      max_index_seen = std::max(max_index_seen, index);
      ++index;
      any = true;
      continue;
    }

    if (bound && index >= *bound) {
      if (outer_braces) {
        sema_.Diag(item->GetBeginLoc(), diag::err_excess_initializers)
            << "array";
        // Consume the rest of the list.
        cur.pos = cur.items.size();
      }
      break;
    }

    Expr* sub = CheckSubobject(elem, cur);
    place(index, sub);
    max_index_seen = std::max(max_index_seen, index);
    ++index;
    any = true;
  }

  // Complete an incomplete array type from the highest initialized index.
  if (!bound) {
    uint64_t size = any ? max_index_seen + 1 : 0;
    type = ctx_.GetConstantArrayType(elem, size)
               .WithQualifiers(type.GetQualifiers());
    semantic.resize(size, nullptr);
  } else {
    semantic.resize(*bound, nullptr);
  }

  auto* result = ctx_.New<InitListExpr>(std::move(semantic), SourceRange{});
  result->SetType(type);
  return result;
}

Expr* InitListChecker::CheckRecord(QualType type, Cursor& cur) {
  QualType canon = type.GetCanonical();
  const auto* rt = canon.GetTypePtr()->As<RecordType>();
  const RecordDecl* record = rt->GetDecl();
  const std::vector<FieldDecl*>& fields = record->GetFields();
  bool is_union = record->IsUnion();

  std::vector<const Expr*> semantic;
  if (!is_union) semantic.resize(fields.size(), nullptr);
  const FieldDecl* union_field = nullptr;

  std::size_t field_index = 0;
  while (!cur.AtEnd()) {
    const Expr* item = cur.Peek();

    if (const auto* de = item->As<DesignatedInitExpr>()) {
      const Designator& first = de->GetDesignators().front();
      if (!first.IsField()) break;  // array designator: not ours

      std::vector<const FieldDecl*> path;
      const FieldDecl* field = record->FindField(first.field, &path);
      if (!field) {
        sema_.Diag(first.loc, diag::err_field_designator_unknown)
            << first.field->GetName() << type.GetAsString();
        had_error_ = true;
        ++cur.pos;
        continue;
      }
      ++cur.pos;

      // Reposition at the designated (top-level) field.
      const FieldDecl* top = path[0];
      std::size_t index = std::distance(
          fields.begin(), std::find(fields.begin(), fields.end(), top));

      Expr* sub;
      if (path.size() > 1 || de->GetDesignators().size() > 1) {
        // Descend through anonymous members and remaining designators.
        // Anonymous hops behave like extra field designators.
        std::vector<Designator> full;
        for (std::size_t i = 1; i < path.size(); ++i) {
          Designator hop;
          hop.field = path[i]->GetIdentifier();
          hop.loc = first.loc;
          full.push_back(hop);
        }
        for (std::size_t i = 1; i < de->GetDesignators().size(); ++i) {
          full.push_back(de->GetDesignators()[i]);
        }
        sub = CheckDesignatedSubobject(top->GetType(), full, 0, de->GetInit());
      } else {
        sub = CheckLeaf(top->GetType(), de->GetInit());
      }

      if (is_union) {
        semantic.assign(1, sub);
        union_field = top;
      } else {
        if (semantic[index]) {
          sema_.Diag(item->GetBeginLoc(), diag::err_initializer_overrides);
        }
        semantic[index] = sub;
      }
      field_index = index + 1;
      if (is_union) break;
      continue;
    }

    if (field_index >= fields.size() || (is_union && field_index > 0)) break;

    const FieldDecl* field = fields[field_index];
    // An unnamed bit-field is not initialized (C11 6.7.9p9).
    if (field->IsBitField() && !field->GetIdentifier()) {
      ++field_index;
      continue;
    }
    // A flexible array member is not initialized.
    if (field->GetType()
            .GetCanonical()
            .GetTypePtr()
            ->As<IncompleteArrayType>()) {
      break;
    }

    Expr* sub = CheckSubobject(field->GetType(), cur);
    if (is_union) {
      semantic.assign(1, sub);
      union_field = field;
      ++field_index;
      break;
    }
    semantic[field_index] = sub;
    ++field_index;
  }

  auto* result = ctx_.New<InitListExpr>(std::move(semantic), SourceRange{});
  result->SetType(type);
  if (union_field) result->SetInitializedField(union_field);
  return result;
}

Expr* InitListChecker::CheckScalar(QualType type, Cursor& cur) {
  if (cur.AtEnd()) {
    had_error_ = true;
    return nullptr;
  }
  const Expr* item = cur.Peek();
  ++cur.pos;
  if (const auto* de = item->As<DesignatedInitExpr>()) {
    sema_.Diag(de->GetDesignators().front().loc,
               diag::err_designator_for_scalar_init)
        << type.GetAsString();
    had_error_ = true;
    return nullptr;
  }
  if (const auto* nested = item->As<InitListExpr>()) {
    // `int x = {{3}}` — too many braces, but recoverable.
    sema_.Diag(item->GetBeginLoc(), diag::err_init_scalar_with_braces);
    Cursor sub_cur{nested->GetInits()};
    return CheckScalar(type, sub_cur);
  }
  return CheckLeaf(type, item);
}

Expr* InitListChecker::CheckExplicitInitList(QualType& type,
                                             InitListExpr* ilist) {
  Cursor cur{ilist->GetInits()};
  QualType canon = type.GetCanonical();

  Expr* result;
  if (canon.GetTypePtr()->IsArrayType()) {
    result = CheckArray(type, cur, /*outer_braces=*/true);
  } else if (canon.GetTypePtr()->IsRecordType()) {
    if (!canon.GetTypePtr()->IsCompleteType()) {
      sema_.Diag(ilist->GetBeginLoc(),
                 diag::err_variable_incomplete_type)
          << type.GetAsString();
      had_error_ = true;
      return ilist;
    }
    result = CheckRecord(type, cur);
  } else {
    if (ilist->GetInits().empty()) {
      sema_.Diag(ilist->GetBeginLoc(), diag::err_init_empty_scalar);
      had_error_ = true;
      return ilist;
    }
    result = CheckScalar(type, cur);
    if (!result) return ilist;
  }

  if (!cur.AtEnd() && !had_error_) {
    const char* what = canon.GetTypePtr()->IsArrayType()
                           ? "array"
                           : (canon.GetTypePtr()->IsUnionType()
                                  ? "union"
                                  : (canon.GetTypePtr()->IsStructType()
                                         ? "struct"
                                         : "scalar"));
    sema_.Diag(cur.Peek()->GetBeginLoc(), diag::err_excess_initializers)
        << what;
  }

  if (result) result->SetSourceRange(ilist->GetSourceRange());
  return result;
}

}  // namespace

ExprResult Sema::CheckInitializer(QualType& type, Expr* init,
                                  bool is_static_storage) {
  if (!init) return ExprError();

  if (auto* ilist = init->As<InitListExpr>()) {
    InitListChecker checker(*this, is_static_storage);
    Expr* result = checker.CheckExplicitInitList(type, ilist);
    if (checker.HadError()) return ExprError();
    return result;
  }

  QualType canon = type.GetCanonical();

  // Array initialized by a string literal (no braces).
  if (const auto* at = canon.GetTypePtr()->As<ArrayType>()) {
    const Expr* inner = init->IgnoreParens();
    const auto* sl = inner->As<StringLiteral>();
    if (!sl || !StringLiteralFitsArray(ctx_, sl, at->GetElementType())) {
      Diag(init->GetBeginLoc(), diag::err_array_init_not_init_list);
      return ExprError();
    }
    uint64_t needed = sl->GetLength() + 1;
    if (const auto* cat = at->As<ConstantArrayType>()) {
      if (sl->GetLength() > cat->GetSize()) {
        Diag(init->GetBeginLoc(), diag::err_array_init_string_too_long);
      }
    } else {
      type = ctx_.GetConstantArrayType(at->GetElementType(), needed)
                 .WithQualifiers(type.GetQualifiers());
    }
    return const_cast<Expr*>(inner);
  }

  QualType init_type = init->GetType();
  AssignConvertType result =
      CheckSingleAssignmentConstraints(type, init, &init_type);
  DiagnoseAssignmentResult(result, init->GetBeginLoc(), type, init_type,
                           "initializing");
  if (result == AssignConvertType::kIncompatible) return ExprError();

  if (is_static_storage && !IsConstantInitializer(*this, init)) {
    Diag(init->GetBeginLoc(), diag::err_init_element_not_constant);
    return ExprError();
  }
  return init;
}

}  // namespace bcc
