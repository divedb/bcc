#include "bcc/ast/ast_context.hh"
#include "bcc/parse/parser.hh"
#include "bcc/pp/identifier_table.hh"

namespace bcc {

//===----------------------------------------------------------------------===//
// External declarations.
//===----------------------------------------------------------------------===//

void Parser::ParseExternalDeclaration() {
  TranslationUnitDecl* tu = sema_.GetASTContext().GetTranslationUnit();

  switch (tok_.GetKind()) {
    case TokenKind::kSemi:
      // Stray ';' at file scope: tolerated (C11 makes it a constraint
      // violation; Clang warns by default).
      ConsumeToken();
      return;
    case TokenKind::kStaticAssert: {
      std::vector<Decl*> decls;
      ParseStaticAssertDeclaration(&decls);
      for (Decl* d : decls) tu->AddDecl(d);
      return;
    }
    default:
      break;
  }

  if (!IsDeclarationSpecifier()) {
    Diag(tok_, diag::err_expected) << "declaration";
    SkipUntil({TokenKind::kSemi});
    return;
  }

  DeclSpec ds;
  ParseDeclarationSpecifiers(ds);

  if (TokIs(TokenKind::kSemi)) {
    SourceLocation semi_loc = ConsumeToken();
    DeclResult r =
        sema_.ActOnEmptyDeclaration(sema_.GetCurScope(), ds, semi_loc);
    if (r.IsUsable()) tu->AddDecl(r.Get());
    return;
  }

  SourceLocation decl_end;
  std::vector<Decl*> decls =
      ParseDeclGroup(ds, DeclaratorContext::kFile, &decl_end,
                     /*allow_function_def=*/true);
  for (Decl* d : decls) tu->AddDecl(d);
}

//===----------------------------------------------------------------------===//
// Declaration specifiers.
//===----------------------------------------------------------------------===//

bool Parser::IsDeclarationSpecifier() {
  switch (tok_.GetKind()) {
    case TokenKind::kTypedef:
    case TokenKind::kExtern:
    case TokenKind::kStatic:
    case TokenKind::kAuto:
    case TokenKind::kRegister:
    case TokenKind::kInline:
    case TokenKind::kNoreturn:
    case TokenKind::kAlignas:
      return true;
    default:
      return IsTypeSpecifierStart(tok_);
  }
}

bool Parser::IsTypeSpecifierStart(const Token& tok) {
  switch (tok.GetKind()) {
    case TokenKind::kVoid:
    case TokenKind::kChar:
    case TokenKind::kShort:
    case TokenKind::kInt:
    case TokenKind::kLong:
    case TokenKind::kFloat:
    case TokenKind::kDouble:
    case TokenKind::kSigned:
    case TokenKind::kUnsigned:
    case TokenKind::kBool:
    case TokenKind::kComplex:
    case TokenKind::kStruct:
    case TokenKind::kUnion:
    case TokenKind::kEnum:
    case TokenKind::kConst:
    case TokenKind::kVolatile:
    case TokenKind::kRestrict:
    case TokenKind::kAtomic:
      return true;
    case TokenKind::kIdentifier:
      return sema_.IsTypeName(tok.GetIdentifierInfo());
    default:
      return false;
  }
}

void Parser::ParseDeclarationSpecifiers(DeclSpec& ds) {
  using SCS = DeclSpec::SCS;
  using TSW = DeclSpec::TSW;
  using TSS = DeclSpec::TSS;
  using TST = DeclSpec::TST;

  auto report = [&](SourceLocation loc, std::string_view spelling,
                    std::string_view prev) {
    if (spelling == prev) {
      Diag(loc, diag::err_duplicate_declspec) << spelling;
    } else {
      Diag(loc, diag::err_invalid_decl_spec_combination) << prev;
    }
  };

  for (;;) {
    std::string_view prev;
    SourceLocation loc = tok_.GetLocation();

    switch (tok_.GetKind()) {
      // Storage-class specifiers.
      case TokenKind::kTypedef:
        if (!ds.SetStorageClass(SCS::kTypedef, loc, prev)) {
          report(loc, "typedef", prev);
        }
        break;
      case TokenKind::kExtern:
        if (!ds.SetStorageClass(SCS::kExtern, loc, prev)) {
          report(loc, "extern", prev);
        }
        break;
      case TokenKind::kStatic:
        if (!ds.SetStorageClass(SCS::kStatic, loc, prev)) {
          report(loc, "static", prev);
        }
        break;
      case TokenKind::kAuto:
        if (!ds.SetStorageClass(SCS::kAuto, loc, prev)) {
          report(loc, "auto", prev);
        }
        break;
      case TokenKind::kRegister:
        if (!ds.SetStorageClass(SCS::kRegister, loc, prev)) {
          report(loc, "register", prev);
        }
        break;

      // Function specifiers.
      case TokenKind::kInline:
        ds.SetInline(loc);
        break;
      case TokenKind::kNoreturn:
        ds.SetNoreturn(loc);
        break;

      // Type qualifiers.
      case TokenKind::kConst:
        ds.SetTypeQual(Qualifiers::kConst, loc);
        break;
      case TokenKind::kVolatile:
        ds.SetTypeQual(Qualifiers::kVolatile, loc);
        break;
      case TokenKind::kRestrict:
        ds.SetTypeQual(Qualifiers::kRestrict, loc);
        break;
      case TokenKind::kAtomic:
        ds.SetTypeQual(Qualifiers::kAtomic, loc);
        break;

      // _Alignas ( type-name | constant-expression ): parsed and ignored.
      case TokenKind::kAlignas: {
        ConsumeToken();
        if (TokIs(TokenKind::kLParen)) {
          ConsumeToken();
          SkipUntil({TokenKind::kRParen});
        } else {
          Diag(tok_, diag::err_expected) << "'('";
        }
        continue;
      }

      // Type-specifier width/sign.
      case TokenKind::kShort:
        if (!ds.SetTypeSpecWidth(TSW::kShort, loc, prev)) {
          report(loc, "short", prev);
        }
        break;
      case TokenKind::kLong:
        if (!ds.SetTypeSpecWidth(TSW::kLong, loc, prev)) {
          report(loc, "long", prev);
        }
        break;
      case TokenKind::kSigned:
        if (!ds.SetTypeSpecSign(TSS::kSigned, loc, prev)) {
          report(loc, "signed", prev);
        }
        break;
      case TokenKind::kUnsigned:
        if (!ds.SetTypeSpecSign(TSS::kUnsigned, loc, prev)) {
          report(loc, "unsigned", prev);
        }
        break;

      // Type specifiers.
      case TokenKind::kVoid:
        if (!ds.SetTypeSpecType(TST::kVoid, loc, prev)) {
          report(loc, "void", prev);
        }
        break;
      case TokenKind::kChar:
        if (!ds.SetTypeSpecType(TST::kChar, loc, prev)) {
          report(loc, "char", prev);
        }
        break;
      case TokenKind::kInt:
        if (!ds.SetTypeSpecType(TST::kInt, loc, prev)) {
          report(loc, "int", prev);
        }
        break;
      case TokenKind::kFloat:
        if (!ds.SetTypeSpecType(TST::kFloat, loc, prev)) {
          report(loc, "float", prev);
        }
        break;
      case TokenKind::kDouble:
        if (!ds.SetTypeSpecType(TST::kDouble, loc, prev)) {
          report(loc, "double", prev);
        }
        break;
      case TokenKind::kBool:
        if (!ds.SetTypeSpecType(TST::kBool, loc, prev)) {
          report(loc, "_Bool", prev);
        }
        break;

      case TokenKind::kStruct:
      case TokenKind::kUnion: {
        bool is_union = TokIs(TokenKind::kUnion);
        SourceLocation kw_loc = ConsumeToken();
        ParseStructUnionSpecifier(ds, kw_loc, is_union);
        continue;
      }
      case TokenKind::kEnum: {
        SourceLocation kw_loc = ConsumeToken();
        ParseEnumSpecifier(ds, kw_loc);
        continue;
      }

      case TokenKind::kIdentifier: {
        // A typedef-name acts as a type specifier — but only when no type
        // specifier has been seen yet (`T x;` vs `int T;`).
        if (ds.HasTypeSpecifier()) return;
        const IdentifierInfo* name = tok_.GetIdentifierInfo();
        if (!sema_.IsTypeName(name)) return;
        NamedDecl* d = sema_.LookupOrdinaryName(name);
        if (!ds.SetTypeSpecType(TST::kTypedefName, loc, prev)) {
          report(loc, name->GetName(), prev);
        } else {
          ds.SetTypedefDecl(d->As<TypedefDecl>());
        }
        break;
      }

      default:
        return;
    }
    ConsumeToken();
  }
}

//===----------------------------------------------------------------------===//
// struct / union / enum specifiers.
//===----------------------------------------------------------------------===//

void Parser::ParseStructUnionSpecifier(DeclSpec& ds, SourceLocation kw_loc,
                                       bool is_union) {
  const IdentifierInfo* name = nullptr;
  SourceLocation name_loc;
  if (TokIs(TokenKind::kIdentifier)) {
    name = tok_.GetIdentifierInfo();
    name_loc = ConsumeToken();
  }

  Sema::TagUseKind use;
  if (TokIs(TokenKind::kLBrace)) {
    use = Sema::TagUseKind::kDefinition;
  } else if (TokIs(TokenKind::kSemi)) {
    use = Sema::TagUseKind::kDeclaration;
  } else {
    use = Sema::TagUseKind::kReference;
  }

  if (!name && use != Sema::TagUseKind::kDefinition) {
    Diag(kw_loc, diag::err_illegal_decl_no_name)
        << (is_union ? "union" : "struct");
    return;
  }

  TagKind kind = is_union ? TagKind::kUnion : TagKind::kStruct;
  TagDecl* tag =
      sema_.ActOnTag(sema_.GetCurScope(), kind, use, kw_loc, name, name_loc);

  if (TokIs(TokenKind::kLBrace)) {
    sema_.ActOnTagStartDefinition(tag);
    BalancedDelimiterTracker braces(*this, TokenKind::kLBrace);
    braces.ConsumeOpen();
    ParseStructDeclarationList(tag->As<RecordDecl>(), sema_.GetCurScope());
    braces.ConsumeClose();
    sema_.ActOnTagFinishDefinition(tag, braces.GetCloseLocation());
  }

  std::string_view prev;
  if (!ds.SetTypeSpecType(is_union ? DeclSpec::TST::kUnion
                                   : DeclSpec::TST::kStruct,
                          kw_loc, prev)) {
    Diag(kw_loc, diag::err_invalid_decl_spec_combination) << prev;
    return;
  }
  ds.SetTagDecl(tag);
}

void Parser::ParseStructDeclarationList(RecordDecl* record,
                                        Scope* member_scope) {
  while (!TokIs(TokenKind::kRBrace) && !TokIs(TokenKind::kEOF)) {
    if (TokIs(TokenKind::kSemi)) {
      ConsumeToken();  // stray ';' between members
      continue;
    }
    if (TokIs(TokenKind::kStaticAssert)) {
      ParseStaticAssertDeclaration(nullptr);
      continue;
    }

    if (!IsDeclarationSpecifier()) {
      Diag(tok_, diag::err_expected) << "member declaration";
      SkipUntil({TokenKind::kSemi});
      continue;
    }

    DeclSpec ds;
    ParseDeclarationSpecifiers(ds);

    // `struct { ... };` with no declarator: an anonymous struct/union member
    // (C11 6.7.2.1p13), or a member-less tag declaration.
    if (TokIs(TokenKind::kSemi)) {
      ConsumeToken();
      TagDecl* inner = ds.GetTagDecl();
      if (inner && !inner->GetIdentifier() &&
          inner->GetKind() == DeclKind::kRecord) {
        Declarator anon(ds, DeclaratorContext::kMember);
        sema_.ActOnField(member_scope, record, anon, nullptr);
      } else if (!inner) {
        Diag(ds.GetBeginLoc(), diag::err_declaration_does_not_declare_anything);
      }
      continue;
    }

    for (;;) {
      Declarator d(ds, DeclaratorContext::kMember);
      if (!TokIs(TokenKind::kColon)) ParseDeclarator(d);

      Expr* bit_width = nullptr;
      if (TryConsumeToken(TokenKind::kColon)) {
        ExprResult width = ParseConstantExpression();
        if (width.IsUsable()) bit_width = width.Get();
      }

      sema_.ActOnField(member_scope, record, d, bit_width);

      if (!TryConsumeToken(TokenKind::kComma)) break;
    }
    ExpectAndConsumeSemi(diag::err_expected_semi_declaration);
  }
}

void Parser::ParseEnumSpecifier(DeclSpec& ds, SourceLocation kw_loc) {
  const IdentifierInfo* name = nullptr;
  SourceLocation name_loc;
  if (TokIs(TokenKind::kIdentifier)) {
    name = tok_.GetIdentifierInfo();
    name_loc = ConsumeToken();
  }

  Sema::TagUseKind use;
  if (TokIs(TokenKind::kLBrace)) {
    use = Sema::TagUseKind::kDefinition;
  } else if (TokIs(TokenKind::kSemi)) {
    use = Sema::TagUseKind::kDeclaration;
  } else {
    use = Sema::TagUseKind::kReference;
  }

  if (!name && use != Sema::TagUseKind::kDefinition) {
    Diag(kw_loc, diag::err_illegal_decl_no_name) << "enum";
    return;
  }

  TagDecl* tag = sema_.ActOnTag(sema_.GetCurScope(), TagKind::kEnum, use,
                                kw_loc, name, name_loc);

  if (TokIs(TokenKind::kLBrace)) {
    sema_.ActOnTagStartDefinition(tag);
    ParseEnumBody(tag->As<EnumDecl>());
    sema_.ActOnTagFinishDefinition(tag, tok_.GetLocation());
  }

  std::string_view prev;
  if (!ds.SetTypeSpecType(DeclSpec::TST::kEnum, kw_loc, prev)) {
    Diag(kw_loc, diag::err_invalid_decl_spec_combination) << prev;
    return;
  }
  ds.SetTagDecl(tag);
}

void Parser::ParseEnumBody(EnumDecl* enum_decl) {
  BalancedDelimiterTracker braces(*this, TokenKind::kLBrace);
  braces.ConsumeOpen();

  EnumConstantDecl* last = nullptr;
  while (!TokIs(TokenKind::kRBrace) && !TokIs(TokenKind::kEOF)) {
    if (!TokIs(TokenKind::kIdentifier)) {
      Diag(tok_, diag::err_expected_ident);
      SkipUntil({TokenKind::kRBrace}, kStopBeforeMatch);
      break;
    }
    const IdentifierInfo* name = tok_.GetIdentifierInfo();
    SourceLocation id_loc = ConsumeToken();

    Expr* value = nullptr;
    if (TryConsumeToken(TokenKind::kEqual)) {
      ExprResult v = ParseConstantExpression();
      if (v.IsUsable()) value = v.Get();
    }

    EnumConstantDecl* ec = sema_.ActOnEnumConstant(
        sema_.GetCurScope(), enum_decl, last, id_loc, name, value);
    if (ec) last = ec;

    if (!TryConsumeToken(TokenKind::kComma)) break;
    // A trailing comma before '}' is valid C99/C11.
  }

  braces.ConsumeClose();
}

//===----------------------------------------------------------------------===//
// _Static_assert.
//===----------------------------------------------------------------------===//

void Parser::ParseStaticAssertDeclaration(std::vector<Decl*>* out) {
  SourceLocation kw_loc = ConsumeToken();  // _Static_assert

  BalancedDelimiterTracker parens(*this, TokenKind::kLParen);
  if (parens.ConsumeOpen()) {
    Diag(tok_, diag::err_expected) << "'('";
    SkipUntil({TokenKind::kSemi});
    return;
  }

  ExprResult cond = ParseConstantExpression();

  std::string message;
  if (TryConsumeToken(TokenKind::kComma)) {
    if (IsStringLiteralKind(tok_.GetKind())) {
      ExprResult msg = ParseStringLiteralExpression();
      if (msg.IsUsable()) {
        if (const auto* sl = msg.Get()->As<StringLiteral>()) {
          message = std::string(sl->GetBytes());
        }
      }
    } else {
      Diag(tok_, diag::err_static_assert_expected_string);
      SkipUntil({TokenKind::kRParen}, kStopBeforeMatch);
    }
  }

  parens.ConsumeClose();
  ExpectAndConsumeSemi(diag::err_expected_semi_declaration);

  if (!cond.IsUsable()) return;
  DeclResult r = sema_.ActOnStaticAssert(kw_loc, cond.Get(),
                                         std::move(message));
  if (out && r.IsUsable()) out->push_back(r.Get());
}

//===----------------------------------------------------------------------===//
// Declaration groups and simple declarations.
//===----------------------------------------------------------------------===//

std::vector<Decl*> Parser::ParseDeclGroup(DeclSpec& ds,
                                          DeclaratorContext context,
                                          SourceLocation* decl_end,
                                          bool allow_function_def) {
  std::vector<Decl*> decls;

  Declarator d(ds, context);
  ParseDeclarator(d);

  // Function definition: a function declarator directly followed by '{'.
  if (allow_function_def && d.IsFunctionDeclarator() && !d.GetChunks().empty()) {
    if (TokIs(TokenKind::kLBrace)) {
      if (Decl* fn = ParseFunctionDefinition(d)) decls.push_back(fn);
      if (decl_end) *decl_end = tok_.GetLocation();
      return decls;
    }
    // K&R parameter declarations between ')' and '{'.
    if (IsDeclarationSpecifier() && !TokIs(TokenKind::kSemi)) {
      Diag(tok_, diag::err_knr_definition_unsupported);
      SkipUntil({TokenKind::kLBrace}, kStopBeforeMatch);
      if (TokIs(TokenKind::kLBrace)) {
        ConsumeToken();
        SkipUntil({TokenKind::kRBrace});
      }
      return decls;
    }
  }

  auto process_declarator = [&](Declarator& declarator) {
    DeclResult r = sema_.ActOnDeclarator(sema_.GetCurScope(), declarator);

    if (TokIs(TokenKind::kEqual)) {
      ConsumeToken();
      ExprResult init = ParseInitializer();
      if (r.IsUsable() && init.IsUsable()) {
        sema_.AddInitializerToDecl(r.Get(), init.Get());
      }
    }

    if (r.IsUsable()) {
      sema_.FinalizeDeclaration(r.Get());
      decls.push_back(r.Get());
    }
  };

  process_declarator(d);
  while (TryConsumeToken(TokenKind::kComma)) {
    Declarator next(ds, context);
    ParseDeclarator(next);
    process_declarator(next);
  }

  if (decl_end) *decl_end = tok_.GetLocation();

  if (TokIs(TokenKind::kLBrace) && d.IsFunctionDeclarator()) {
    // `int f(), g() {}` — a body after a declarator list.
    Diag(tok_, diag::err_expected_fn_body);
    ConsumeToken();
    SkipUntil({TokenKind::kRBrace});
    return decls;
  }

  ExpectAndConsumeSemi(diag::err_expected_semi_declaration);
  return decls;
}

std::vector<Decl*> Parser::ParseSimpleDeclaration(DeclaratorContext context,
                                                  SourceLocation* decl_end) {
  if (TokIs(TokenKind::kStaticAssert)) {
    std::vector<Decl*> decls;
    ParseStaticAssertDeclaration(&decls);
    if (decl_end) *decl_end = tok_.GetLocation();
    return decls;
  }

  DeclSpec ds;
  ParseDeclarationSpecifiers(ds);

  if (TokIs(TokenKind::kSemi)) {
    SourceLocation semi_loc = ConsumeToken();
    if (decl_end) *decl_end = semi_loc;
    DeclResult r =
        sema_.ActOnEmptyDeclaration(sema_.GetCurScope(), ds, semi_loc);
    std::vector<Decl*> decls;
    if (r.IsUsable()) decls.push_back(r.Get());
    return decls;
  }

  return ParseDeclGroup(ds, context, decl_end, /*allow_function_def=*/false);
}

//===----------------------------------------------------------------------===//
// Declarators.
//===----------------------------------------------------------------------===//

void Parser::ParseDeclarator(Declarator& d) {
  if (TokIs(TokenKind::kStar)) {
    SourceLocation star_loc = ConsumeToken();
    uint8_t quals = 0;
    for (;;) {
      if (TokIs(TokenKind::kConst)) {
        quals |= Qualifiers::kConst;
      } else if (TokIs(TokenKind::kVolatile)) {
        quals |= Qualifiers::kVolatile;
      } else if (TokIs(TokenKind::kRestrict)) {
        quals |= Qualifiers::kRestrict;
      } else if (TokIs(TokenKind::kAtomic)) {
        quals |= Qualifiers::kAtomic;
      } else {
        break;
      }
      ConsumeToken();
    }
    ParseDeclarator(d);
    // Pointer chunks wrap everything parsed on the inside, so they are added
    // after the inner chunks (outermost last).
    d.AddChunk(DeclaratorChunk::MakePointer(quals, star_loc));
    return;
  }
  ParseDirectDeclarator(d);
}

void Parser::ParseDirectDeclarator(Declarator& d) {
  if (TokIs(TokenKind::kIdentifier)) {
    const IdentifierInfo* id = tok_.GetIdentifierInfo();
    if (d.GetContext() != DeclaratorContext::kTypeName) {
      d.SetIdentifier(id, ConsumeToken());
    } else {
      // A type-name has no declarator-id; an identifier here is an error the
      // caller will report when it fails to find the closing token.
      Diag(tok_, diag::err_unexpected_typedef_ident) << id->GetName();
      ConsumeToken();
      d.SetInvalid();
    }
  } else if (TokIs(TokenKind::kLParen)) {
    ParseParenDeclarator(d);
  } else {
    // No identifier: fine for abstract declarators (type-names, parameters);
    // otherwise the declaration is missing its name — the caller diagnoses
    // via Sema (declaration does not declare anything) or here for members.
    d.SetIdentifier(nullptr, tok_.GetLocation());
  }

  // Postfix declarator chunks bind tighter than any pointer prefix; they are
  // added first (innermost first).
  for (;;) {
    if (TokIs(TokenKind::kLParen)) {
      SourceLocation lparen = ConsumeToken();
      ParseFunctionDeclarator(d, lparen);
    } else if (TokIs(TokenKind::kLSquare)) {
      ParseBracketDeclarator(d);
    } else {
      return;
    }
  }
}

void Parser::ParseParenDeclarator(Declarator& d) {
  SourceLocation lparen = ConsumeToken();

  // '(' in a direct-declarator is either a grouping paren or a parameter
  // list of an omitted-name function declarator (abstract declarators like
  // `int(int)`). It is a parameter list if it starts with ')' or a
  // declaration specifier.
  if (TokIs(TokenKind::kRParen) || IsDeclarationSpecifier()) {
    d.SetIdentifier(nullptr, lparen);
    ParseFunctionDeclarator(d, lparen);
    return;
  }

  ParseDeclarator(d);

  if (!TryConsumeToken(TokenKind::kRParen)) {
    Diag(tok_, diag::err_expected_rparen);
    Diag(lparen, diag::note_to_match_this_lparen);
    SkipUntil({TokenKind::kRParen});
  }
}

void Parser::ParseFunctionDeclarator(Declarator& d, SourceLocation lparen) {
  std::vector<DeclaratorChunk::ParamInfo> params;
  bool is_variadic = false;
  bool has_proto = false;

  ParseScope proto_scope(this, Scope::kFnProto | Scope::kDecl);

  if (TokIs(TokenKind::kRParen)) {
    // `()` — unprototyped.
  } else if (TokIs(TokenKind::kIdentifier) &&
             !sema_.IsTypeName(tok_.GetIdentifierInfo())) {
    // K&R identifier list `f(a, b)`: parsed, but definitions using it are
    // rejected later; the type is unprototyped.
    while (TokIs(TokenKind::kIdentifier)) {
      ConsumeToken();
      if (!TryConsumeToken(TokenKind::kComma)) break;
    }
  } else {
    has_proto = true;
    ParseParameterDeclarationClause(params, is_variadic, proto_scope.Get());
  }

  if (!TryConsumeToken(TokenKind::kRParen)) {
    Diag(tok_, diag::err_expected_rparen);
    Diag(lparen, diag::note_to_match_this_lparen);
    SkipUntil({TokenKind::kRParen});
  }

  d.AddChunk(DeclaratorChunk::MakeFunction(has_proto, is_variadic,
                                           std::move(params), lparen));
}

void Parser::ParseParameterDeclarationClause(
    std::vector<DeclaratorChunk::ParamInfo>& params, bool& is_variadic,
    Scope* proto_scope) {
  bool first = true;
  for (;;) {
    if (TokIs(TokenKind::kEllipsis)) {
      ConsumeToken();
      is_variadic = true;
      return;
    }

    if (!IsDeclarationSpecifier()) {
      Diag(tok_, diag::err_parameter_requires_type);
      SkipUntil({TokenKind::kRParen}, kStopBeforeMatch);
      return;
    }

    DeclSpec ds;
    ParseDeclarationSpecifiers(ds);
    Declarator d(ds, DeclaratorContext::kPrototype);
    ParseDeclarator(d);

    // `(void)` — a prototype with no parameters (C11 6.7.6.3p10).
    if (first && !d.HasName() && d.GetChunks().empty() &&
        ds.GetTypeSpecType() == DeclSpec::TST::kVoid &&
        ds.GetTypeQuals() == 0 && TokIs(TokenKind::kRParen)) {
      return;
    }
    if (!first && !d.HasName() && d.GetChunks().empty() &&
        ds.GetTypeSpecType() == DeclSpec::TST::kVoid) {
      Diag(ds.GetTypeSpecLoc(), diag::err_void_only_param);
      if (!TryConsumeToken(TokenKind::kComma)) return;
      continue;
    }

    ParmVarDecl* param = sema_.ActOnParamDeclarator(proto_scope, d);
    params.push_back({d.GetIdentifier(), d.GetIdentifierLoc(), param});
    first = false;

    if (!TryConsumeToken(TokenKind::kComma)) return;
  }
}

void Parser::ParseBracketDeclarator(Declarator& d) {
  BalancedDelimiterTracker brackets(*this, TokenKind::kLSquare);
  brackets.ConsumeOpen();

  bool has_static = TryConsumeToken(TokenKind::kStatic);
  uint8_t quals = 0;
  for (;;) {
    if (TokIs(TokenKind::kConst)) {
      quals |= Qualifiers::kConst;
    } else if (TokIs(TokenKind::kVolatile)) {
      quals |= Qualifiers::kVolatile;
    } else if (TokIs(TokenKind::kRestrict)) {
      quals |= Qualifiers::kRestrict;
    } else if (TokIs(TokenKind::kAtomic)) {
      quals |= Qualifiers::kAtomic;
    } else {
      break;
    }
    ConsumeToken();
  }
  if (!has_static) has_static = TryConsumeToken(TokenKind::kStatic);

  Expr* size = nullptr;
  bool is_star = false;
  if (TokIs(TokenKind::kStar) &&
      NextToken().GetKind() == TokenKind::kRSquare) {
    ConsumeToken();
    is_star = true;
  } else if (!TokIs(TokenKind::kRSquare)) {
    ExprResult size_result = ParseAssignmentExpression();
    if (size_result.IsUsable()) {
      size = size_result.Get();
    } else {
      SkipUntil({TokenKind::kRSquare}, kStopBeforeMatch);
    }
  }

  brackets.ConsumeClose();
  d.AddChunk(DeclaratorChunk::MakeArray(size, has_static, is_star, quals,
                                        brackets.GetOpenLocation()));
}

//===----------------------------------------------------------------------===//
// Type names.
//===----------------------------------------------------------------------===//

QualType Parser::ParseTypeName() {
  DeclSpec ds;
  ParseDeclarationSpecifiers(ds);
  if (!ds.HasTypeSpecifier() && ds.GetTypeQuals() == 0) {
    Diag(tok_, diag::err_typename_requires_specqual);
    return {};
  }
  Declarator d(ds, DeclaratorContext::kTypeName);
  ParseDeclarator(d);
  if (d.IsInvalid()) return {};
  return sema_.ActOnTypeName(d);
}

//===----------------------------------------------------------------------===//
// Function definitions.
//===----------------------------------------------------------------------===//

Decl* Parser::ParseFunctionDefinition(Declarator& d) {
  ParseScope fn_scope(this, Scope::kFn | Scope::kDecl);

  FunctionDecl* fd = sema_.ActOnStartOfFunctionDef(fn_scope.Get(), d);

  StmtResult body = ParseCompoundStatement(Scope::kDecl);

  sema_.ActOnFinishFunctionBody(fd, body.IsUsable() ? body.Get() : nullptr);
  return fd;
}

}  // namespace bcc
