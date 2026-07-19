#include "bcc/pp/identifier_table.hh"
#include "bcc/sema/sema.hh"

namespace bcc {

//===----------------------------------------------------------------------===//
// DeclSpec -> QualType (Clang: SemaType.cpp ConvertDeclSpecToType).
//===----------------------------------------------------------------------===//

QualType Sema::ConvertDeclSpecToType(const DeclSpec& ds) {
  using TST = DeclSpec::TST;
  using TSW = DeclSpec::TSW;
  using TSS = DeclSpec::TSS;

  TST tst = ds.GetTypeSpecType();
  TSW tsw = ds.GetTypeSpecWidth();
  TSS tss = ds.GetTypeSpecSign();
  SourceLocation loc = ds.GetBeginLoc();

  auto reject_sign = [&](std::string_view what) {
    if (tss != TSS::kUnspecified) {
      Diag(loc, diag::err_invalid_sign_spec) << what;
    }
  };
  auto reject_width = [&](std::string_view what) {
    if (tsw != TSW::kUnspecified) {
      Diag(loc, diag::err_invalid_width_spec) << what;
    }
  };

  QualType result;
  bool is_unsigned = tss == TSS::kUnsigned;

  switch (tst) {
    case TST::kUnspecified:
      // `unsigned x;` / `long x;` are int with modifiers; a bare declarator
      // with no type specifier at all is implicit int (diagnosed).
      if (tsw == TSW::kUnspecified && tss == TSS::kUnspecified) {
        Diag(loc, diag::warn_missing_type_specifier);
      }
      [[fallthrough]];
    case TST::kInt:
      switch (tsw) {
        case TSW::kUnspecified:
          result = is_unsigned ? ctx_.UIntTy() : ctx_.IntTy();
          break;
        case TSW::kShort:
          result = is_unsigned ? ctx_.UShortTy() : ctx_.ShortTy();
          break;
        case TSW::kLong:
          result = is_unsigned ? ctx_.ULongTy() : ctx_.LongTy();
          break;
        case TSW::kLongLong:
          result = is_unsigned ? ctx_.ULongLongTy() : ctx_.LongLongTy();
          break;
      }
      break;
    case TST::kChar:
      reject_width("char");
      if (tss == TSS::kUnspecified) {
        result = ctx_.CharTy();
      } else {
        result = is_unsigned ? ctx_.UCharTy() : ctx_.SCharTy();
      }
      break;
    case TST::kVoid:
      reject_sign("void");
      reject_width("void");
      result = ctx_.VoidTy();
      break;
    case TST::kBool:
      reject_sign("_Bool");
      reject_width("_Bool");
      result = ctx_.BoolTy();
      break;
    case TST::kFloat:
      reject_sign("float");
      reject_width("float");
      result = ctx_.FloatTy();
      break;
    case TST::kDouble:
      reject_sign("double");
      if (tsw == TSW::kLong) {
        result = ctx_.LongDoubleTy();
      } else {
        reject_width("double");
        result = ctx_.DoubleTy();
      }
      break;
    case TST::kStruct:
    case TST::kUnion:
    case TST::kEnum: {
      reject_sign("tag type");
      reject_width("tag type");
      TagDecl* tag = ds.GetTagDecl();
      if (!tag) return {};
      result = ctx_.GetTagType(tag);
      break;
    }
    case TST::kTypedefName: {
      reject_sign(ds.GetTypedefDecl() ? ds.GetTypedefDecl()->GetName()
                                      : "type");
      reject_width(ds.GetTypedefDecl() ? ds.GetTypedefDecl()->GetName()
                                       : "type");
      TypedefDecl* td = ds.GetTypedefDecl();
      if (!td) return {};
      result = ctx_.GetTypedefType(td);
      break;
    }
  }

  if (ds.GetTypeQuals() != 0) {
    result = result.WithQualifiers(Qualifiers(ds.GetTypeQuals()));
  }
  return result;
}

//===----------------------------------------------------------------------===//
// Declarator -> QualType (Clang: GetFullTypeForDeclarator).
//===----------------------------------------------------------------------===//

QualType Sema::GetTypeForDeclarator(Declarator& d) {
  QualType t = ConvertDeclSpecToType(d.GetDeclSpec());
  if (t.IsNull()) {
    d.SetInvalid();
    return {};
  }

  std::string_view name =
      d.HasName() ? d.GetIdentifier()->GetName() : std::string_view("type");

  // Chunks are ordered from the identifier outward; the outermost chunk is
  // applied to the DeclSpec type first.
  auto& chunks = d.GetChunks();
  for (auto it = chunks.rbegin(); it != chunks.rend(); ++it) {
    DeclaratorChunk& chunk = *it;
    switch (chunk.kind) {
      case DeclaratorChunk::Kind::kPointer:
        t = ctx_.GetPointerType(t).WithQualifiers(
            Qualifiers(chunk.pointer_quals));
        break;

      case DeclaratorChunk::Kind::kArray: {
        if (t->IsFunctionType()) {
          Diag(chunk.loc, diag::err_array_of_functions) << name;
          d.SetInvalid();
          return {};
        }
        if (!t->IsCompleteType()) {
          Diag(chunk.loc, diag::err_illegal_decl_array_incomplete_type)
              << t.GetAsString();
          d.SetInvalid();
          return {};
        }
        if (!chunk.array_size) {
          t = ctx_.GetIncompleteArrayType(t);
          break;
        }
        QualType size_type = chunk.array_size->GetType();
        if (!size_type.IsNull() && !size_type->IsIntegerType()) {
          Diag(chunk.loc, diag::err_array_size_non_int)
              << size_type.GetAsString();
          d.SetInvalid();
          return {};
        }
        if (std::optional<ICEValue> size = EvaluateICE(chunk.array_size)) {
          if (!size->is_unsigned && size->value < 0) {
            Diag(chunk.loc, diag::err_typecheck_negative_array_size) << name;
            d.SetInvalid();
            return {};
          }
          if (size->value == 0) {
            Diag(chunk.loc, diag::warn_typecheck_zero_array_size);
          }
          t = ctx_.GetConstantArrayType(t,
                                        static_cast<uint64_t>(size->value));
        } else {
          // Non-constant bound: a variable-length array type.
          t = ctx_.GetVariableArrayType(t, chunk.array_size);
        }
        break;
      }

      case DeclaratorChunk::Kind::kFunction: {
        if (t->IsArrayType() || t->IsFunctionType()) {
          Diag(chunk.loc, diag::err_func_returning_array_function)
              << (t->IsArrayType() ? "array" : "function") << t.GetAsString();
          d.SetInvalid();
          return {};
        }
        if (!chunk.fun_has_proto) {
          t = ctx_.GetFunctionNoProtoType(t);
          break;
        }
        std::vector<QualType> param_types;
        param_types.reserve(chunk.params.size());
        for (const DeclaratorChunk::ParamInfo& p : chunk.params) {
          if (!p.decl) continue;  // erroneous parameter
          param_types.push_back(p.decl->GetType());
        }
        t = ctx_.GetFunctionType(t, std::move(param_types),
                                 chunk.fun_is_variadic);
        break;
      }
    }
  }

  return t;
}

QualType Sema::ActOnTypeName(Declarator& d) {
  QualType t = GetTypeForDeclarator(d);
  if (d.GetDeclSpec().GetStorageClass() != DeclSpec::SCS::kUnspecified) {
    Diag(d.GetDeclSpec().GetStorageClassLoc(),
         diag::err_typename_invalid_storageclass);
  }
  return t;
}

bool Sema::RequireCompleteType(SourceLocation loc, QualType t,
                               diag::DiagKind kind) {
  if (t.IsNull()) return true;
  if (t->IsCompleteType()) return false;
  Diag(loc, kind) << t.GetAsString();
  return true;
}

}  // namespace bcc
