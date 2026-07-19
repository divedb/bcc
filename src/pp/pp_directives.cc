#include <cctype>
#include <cstdio>
#include <string>
#include <string_view>

#include "bcc/basic/diagnostic.hh"
#include "bcc/basic/diagnostic_ids.hh"
#include "bcc/basic/file_manager.hh"
#include "bcc/basic/source_manager.hh"
#include "bcc/common/string_util.hh"
#include "bcc/pp/header_search.hh"
#include "bcc/pp/identifier_table.hh"
#include "bcc/pp/macro_info.hh"
#include "bcc/pp/pp_callbacks.hh"
#include "bcc/pp/preprocessor.hh"

namespace bcc {

void Preprocessor::NoteNonGuardDirectiveAtFileScope() {
  if (cur_lexer_->GetConditionalStackDepth() == 0) {
    cur_lexer_->GetMIOpt().OtherTopLevelDirective();
  }
}

namespace {

std::string CleanTokenSpelling(const Token& token) {
  if (token.NeedsCleaning()) {
    return RemoveLineSplices(token.GetLexeme());
  }
  return std::string(token.GetLexeme());
}

const char* ElifKeywordName(PPKeyword kw) {
  switch (kw) {
    case PPKeyword::kElifdef:
      return "elifdef";
    case PPKeyword::kElifndef:
      return "elifndef";
    default:
      return "elif";
  }
}

// Decodes a string-literal lexeme (with quotes and escapes) to its raw byte
// content. Used by #pragma message/GCC warning/GCC error to concatenate and
// re-encode their string arguments the way Clang's -E output does.
std::string DecodeStringLiteralBytes(std::string_view lexeme) {
  std::string out;
  std::size_t i = 0;
  // Skip an encoding prefix (u8, u, U, L).
  while (i < lexeme.size() && lexeme[i] != '"') ++i;
  ++i;  // past opening quote
  for (; i < lexeme.size() && lexeme[i] != '"'; ++i) {
    if (lexeme[i] != '\\') {
      out += lexeme[i];
      continue;
    }
    ++i;  // consume backslash
    if (i >= lexeme.size()) break;
    char e = lexeme[i];
    auto hexdigit = [](char c) {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return c - 'A' + 10;
    };
    switch (e) {
      case 'n':
        out += '\n';
        break;
      case 't':
        out += '\t';
        break;
      case 'r':
        out += '\r';
        break;
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7': {
        int v = e - '0';
        int count = 1;
        while (count < 3 && i + 1 < lexeme.size() && lexeme[i + 1] >= '0' &&
               lexeme[i + 1] <= '7') {
          v = v * 8 + (lexeme[++i] - '0');
          ++count;
        }
        out += static_cast<char>(v);
        break;
      }
      case 'x': {
        int v = 0;
        while (i + 1 < lexeme.size() &&
               std::isxdigit(static_cast<unsigned char>(lexeme[i + 1]))) {
          v = v * 16 + hexdigit(lexeme[++i]);
        }
        out += static_cast<char>(v);
        break;
      }
      default:
        out += e;
        break;
    }
  }
  return out;
}

// Re-encodes raw bytes the way Clang prints a #pragma message/warning/error
// string: backslash, double-quote, and non-printable bytes become 3-digit
// octal escapes; everything else is literal. The result does NOT include the
// surrounding quotes.
std::string EncodePragmaMessageBytes(std::string_view bytes) {
  std::string out;
  for (unsigned char b : bytes) {
    if (b == '\\') {
      out += "\\134";
    } else if (b == '"') {
      out += "\\042";
    } else if (b < 0x20 || b == 0x7f) {
      char buf[5];
      std::snprintf(buf, sizeof(buf), "\\%03o", b);
      out += buf;
    } else {
      out += static_cast<char>(b);
    }
  }
  return out;
}

}  // namespace

void Preprocessor::HandleDirective(Token& /*hash_tok*/) {
  // The rest of the line is a directive: turn its terminating newline into
  // kEod.
  cur_lexer_->SetParsingPreprocessorDirective(true);

  Token directive = cur_lexer_->Lex();

  // A '#' alone on a line (null directive).
  if (directive.GetKind() == TokenKind::kEod ||
      directive.GetKind() == TokenKind::kEOF) {
    return;
  }

  if (directive.GetKind() != TokenKind::kIdentifier) {
    // e.g. a GNU line marker like '# 12 "file"'; unsupported here.
    DiscardUntilEndOfDirective();
    return;
  }

  IdentifierInfo* ii = LookUpIdentifierInfo(directive);

  switch (ii->GetPPKeyword()) {
    case PPKeyword::kDefine:
      HandleDefineDirective(directive);
      break;
    case PPKeyword::kUndef:
      HandleUndefDirective(directive);
      break;
    case PPKeyword::kIf:
      HandleIfDirective(directive);
      break;
    case PPKeyword::kIfdef:
      HandleIfdefDirective(directive, /*is_ifndef=*/false);
      break;
    case PPKeyword::kIfndef:
      HandleIfdefDirective(directive, /*is_ifndef=*/true);
      break;
    case PPKeyword::kElif:
    case PPKeyword::kElifdef:
    case PPKeyword::kElifndef:
      HandleElifFamilyDirective(directive, ii->GetPPKeyword());
      break;
    case PPKeyword::kElse:
      HandleElseDirective(directive);
      break;
    case PPKeyword::kEndif:
      HandleEndifDirective(directive);
      break;
    case PPKeyword::kInclude:
      HandleIncludeDirective(directive);
      break;
    case PPKeyword::kIncludeNext:
      HandleIncludeNextDirective(directive);
      break;
    case PPKeyword::kImport:
      HandleImportDirective(directive);
      break;
    case PPKeyword::kIncludeMacros:
      HandleIncludeMacrosDirective(directive);
      break;
    case PPKeyword::kPragma:
      HandlePragmaDirective(directive);
      break;
    case PPKeyword::kIdent:
    case PPKeyword::kSccs:
      HandleIdentDirective(directive);
      break;
    case PPKeyword::kLine:
      HandleLineDirective(directive);
      break;
    case PPKeyword::kError:
      HandleUserDiagnosticDirective(directive, /*is_error=*/true);
      break;
    case PPKeyword::kWarning:
      HandleUserDiagnosticDirective(directive, /*is_error=*/false);
      break;
    default:
      // A directive at file scope that we don't model still breaks the include
      // guard shape.
      NoteNonGuardDirectiveAtFileScope();
      DiscardUntilEndOfDirective();
      break;
  }
}

void Preprocessor::HandleDefineDirective(Token& /*define_tok*/) {
  Token name = cur_lexer_->Lex();

  if (name.GetKind() != TokenKind::kIdentifier) {
    // Missing or invalid macro name. Consume the rest of the line unless we
    // are already sitting on its end.
    if (name.GetKind() != TokenKind::kEod &&
        name.GetKind() != TokenKind::kEOF) {
      DiscardUntilEndOfDirective();
    }
    return;
  }

  IdentifierInfo* ii = LookUpIdentifierInfo(name);

  // Include-guard tracking: a `#define GUARD` right after the guard's `#ifndef`
  // (nested inside it) completes the guard; a #define at file scope breaks it.
  if (cur_lexer_->GetConditionalStackDepth() == 0) {
    cur_lexer_->GetMIOpt().OtherTopLevelDirective();
  } else {
    cur_lexer_->GetMIOpt().DefinedMacro(ii);
  }

  MacroInfo* macro = AllocateMacroInfo(name.GetLocation());

  Token tok = cur_lexer_->Lex();  // the token immediately after the name

  // A '(' with no intervening whitespace makes this a function-like macro.
  if (tok.GetKind() == TokenKind::kLParen && !tok.HasLeadingSpace()) {
    macro->SetIsFunctionLike();
    ReadMacroParameterList(macro);
    tok = cur_lexer_->Lex();  // first replacement token
  }

  // Read the replacement list up to (and consuming) the end-of-directive.
  SourceLocation end_loc = name.GetLocation();
  bool first = true;
  bool has_paste = false;
  while (tok.GetKind() != TokenKind::kEod && tok.GetKind() != TokenKind::kEOF) {
    // Attach IdentifierInfo now so the token can be re-expanded during
    // rescanning (and parameter references resolved) without another lookup.
    if (tok.GetKind() == TokenKind::kIdentifier) {
      LookUpIdentifierInfo(tok);
    }
    if (tok.GetKind() == TokenKind::kHashHash) {
      has_paste = true;
    }
    if (first) {
      // The first replacement token carries no leading spacing of its own.
      tok.ClearFlag(TokenFlag::kStartOfLine);
      tok.ClearFlag(TokenFlag::kLeadingSpace);
      first = false;
    }
    end_loc = tok.GetLocation();
    macro->AddReplacementToken(tok);
    tok = cur_lexer_->Lex();
  }
  macro->SetHasPasteOperator(has_paste);
  macro->SetDefinitionEndLoc(end_loc);

  AppendDefMacroDirective(ii, macro, name.GetLocation());
}

void Preprocessor::ReadMacroParameterList(MacroInfo* macro) {
  Token tok = cur_lexer_->Lex();
  if (tok.GetKind() == TokenKind::kRParen) return;  // F()

  for (;;) {
    if (tok.GetKind() == TokenKind::kEllipsis) {
      // C99 variadic: `...` is captured as the __VA_ARGS__ parameter.
      macro->SetIsVariadic(true);
      macro->AddParameter(&GetIdentifierTable().Get("__VA_ARGS__"));
      tok = cur_lexer_->Lex();  // expected ')'
      break;
    }

    if (tok.GetKind() != TokenKind::kIdentifier) break;  // malformed list

    IdentifierInfo* param_ii = LookUpIdentifierInfo(tok);

    tok = cur_lexer_->Lex();
    if (tok.GetKind() == TokenKind::kEllipsis) {
      // GNU named-variadic: `name...` — the identifier is the last named
      // parameter; everything beyond it falls into the trailing variadic slot.
      macro->SetIsVariadic(true);
      macro->AddParameter(param_ii);
      tok = cur_lexer_->Lex();  // expected ')'
      break;
    }

    macro->AddParameter(param_ii);

    if (tok.GetKind() == TokenKind::kComma) {
      tok = cur_lexer_->Lex();
      continue;
    }
    break;  // expected ')'
  }
  // On a well-formed list `tok` is now the ')', already consumed.
}

void Preprocessor::HandleUndefDirective(Token& /*undef_tok*/) {
  Token name = cur_lexer_->Lex();

  if (name.GetKind() != TokenKind::kIdentifier) {
    if (name.GetKind() != TokenKind::kEod &&
        name.GetKind() != TokenKind::kEOF) {
      DiscardUntilEndOfDirective();
    }
    return;
  }

  IdentifierInfo* ii = LookUpIdentifierInfo(name);

  NoteNonGuardDirectiveAtFileScope();

  DiscardUntilEndOfDirective();  // consume any trailing tokens + kEod
  AppendUndefMacroDirective(ii, name.GetLocation());
}

void Preprocessor::DiscardUntilEndOfDirective() {
  Token tok = cur_lexer_->Lex();

  while (tok.GetKind() != TokenKind::kEod && tok.GetKind() != TokenKind::kEOF) {
    tok = cur_lexer_->Lex();
  }
}

//===----------------------------------------------------------------------===//
// Source file inclusion (#include, #pragma once)
//===----------------------------------------------------------------------===//

std::string_view Preprocessor::GetCurrentFileDir() const {
  if (!cur_lexer_) return {};

  std::string_view name = sm_.GetFilename(cur_lexer_->GetFileID());
  std::string_view::size_type slash = name.find_last_of('/');

  if (slash == std::string_view::npos) return {};

  return name.substr(0, slash);
}

void Preprocessor::HandleIncludeDirective(Token& include_tok) {
  HandleIncludeCommon(include_tok, IncludeKind::kInclude);
}

void Preprocessor::HandleIncludeNextDirective(Token& include_tok) {
  HandleIncludeCommon(include_tok, IncludeKind::kIncludeNext);
}

void Preprocessor::HandleImportDirective(Token& import_tok) {
  HandleIncludeCommon(import_tok, IncludeKind::kImport);
}

void Preprocessor::HandleIncludeMacrosDirective(Token& include_tok) {
  HandleIncludeCommon(include_tok, IncludeKind::kIncludeMacros);
}

std::optional<Preprocessor::HeaderName> Preprocessor::ParseHeaderName() {
  Token filename_tok = cur_lexer_->LexIncludeFilename();
  HeaderName header{{}, filename_tok.GetLocation(), false};
  bool consumed_to_eod = false;

  if (filename_tok.GetKind() == TokenKind::kHeaderName) {
    std::string spelling = CleanTokenSpelling(filename_tok);
    std::string_view raw = spelling;
    header.is_angled = raw.front() == '<';
    header.filename = std::string(raw.substr(1, raw.size() - 2));
  } else if (filename_tok.GetKind() == TokenKind::kEod ||
             filename_tok.GetKind() == TokenKind::kEOF) {
    diags_.Report(filename_tok.GetLocation(), diag::err_pp_expected_filename);
    return std::nullopt;
  } else {
    // Computed include: macro-expand the rest of the directive line and
    // interpret the result as a header name. Handles `#include MACRO`,
    // `#include __FILE__`, and `#include FOO(...)`.
    std::vector<Token> toks;
    toks.push_back(filename_tok);

    for (;;) {
      Token t = cur_lexer_->Lex();
      if (t.GetKind() == TokenKind::kEod || t.GetKind() == TokenKind::kEOF)
        break;
      toks.push_back(t);
    }

    consumed_to_eod = true;

    // Attach IdentifierInfo so ExpandArgument can macro-expand each token
    // (e.g. __FILE__, or a function-like macro invocation).
    for (Token& t : toks) {
      if (t.GetKind() == TokenKind::kIdentifier &&
          t.GetIdentifierInfo() == nullptr) {
        LookUpIdentifierInfo(t);
      }
    }

    std::vector<Token> expanded = ExpandArgument(toks);

    if (!InterpretExpandedHeader(expanded, header.filename, header.is_angled)) {
      diags_.Report(filename_tok.GetLocation(), diag::err_pp_expected_filename);
      return std::nullopt;
    }
  }

  if (!consumed_to_eod) DiscardUntilEndOfDirective();

  if (header.filename.empty()) {
    diags_.Report(filename_tok.GetLocation(), diag::err_pp_empty_filename);
    return std::nullopt;
  }

  return header;
}

const FileEntry* Preprocessor::LookupHeader(const HeaderName& header,
                                            IncludeKind kind) {
  if (header_search_ == nullptr) return nullptr;

  if (kind == IncludeKind::kIncludeNext) {
    return header_search_->LookupFileNext(header.filename, header.is_angled,
                                          GetCurrentFileDir());
  }
  return header_search_->LookupFile(header.filename, header.is_angled,
                                    GetCurrentFileDir());
}

bool Preprocessor::ShouldEnterHeader(const FileEntry* file, SourceLocation loc,
                                     IncludeKind kind) {
  const HeaderFileInfo* info = header_search_->GetExistingFileInfo(file);
  if (kind == IncludeKind::kImport) {
    if (info != nullptr && info->is_imported) return false;
  } else if (info != nullptr) {
    if (info->is_pragma_once || (info->controlling_macro != nullptr &&
                                 IsMacroDefined(info->controlling_macro))) {
      return false;
    }
  }

  if (GetIncludeStackDepth() >= kMaxIncludeDepth) {
    diags_.Report(loc, diag::err_pp_include_too_deep);
    return false;
  }
  return true;
}

bool Preprocessor::EnterResolvedHeader(const FileEntry* file,
                                       SourceLocation loc, IncludeKind kind) {
  FileID fid = sm_.CreateFileID(*file, loc);

  if (!fid.IsValid()) return false;

  if (kind == IncludeKind::kImport) {
    header_search_->SetFileImported(file);
  }

  // kIncludeMacros currently enters the file normally, preserving the prior
  // behavior until macros-only token suppression is implemented.
  EnterIncludeFile(fid, loc);

  return true;
}

bool Preprocessor::HandleIncludeCommon(Token& include_tok, IncludeKind kind) {
  NoteNonGuardDirectiveAtFileScope();
  std::optional<HeaderName> header = ParseHeaderName();

  if (!header) return false;

  const FileEntry* file = LookupHeader(*header, kind);
  if (callbacks_) {
    CharacteristicKind file_type =
        file != nullptr ? header_search_->GetFileCharacteristic(file)
                        : CharacteristicKind::kUser;
    callbacks_->InclusionDirective(include_tok.GetLocation(), header->filename,
                                   header->is_angled, file, file_type);
  }

  if (file == nullptr) {
    diags_.Report(header->location, kind == IncludeKind::kIncludeNext
                                        ? diag::err_pp_include_next_not_found
                                    : kind == IncludeKind::kImport
                                        ? diag::err_pp_import_failed
                                        : diag::err_pp_file_not_found)
        << header->filename;

    return false;
  }

  if (!ShouldEnterHeader(file, header->location, kind)) return false;

  if (!EnterResolvedHeader(file, header->location, kind)) {
    diags_.Report(header->location, kind == IncludeKind::kIncludeNext
                                        ? diag::err_pp_include_next_not_found
                                    : kind == IncludeKind::kImport
                                        ? diag::err_pp_import_failed
                                        : diag::err_pp_file_not_found)
        << header->filename;
    return false;
  }

  return true;
}

// Interprets a (macro-expanded) token sequence as a #include header name:
//   * a single string literal  ->  "file"   (quoted)
//   * "<" ... ">"              ->  <file>   (angled)
// Returns false (leaving \p filename empty) if neither shape matches.
bool Preprocessor::InterpretExpandedHeader(const std::vector<Token>& toks,
                                           std::string& filename,
                                           bool& is_angled) {
  if (toks.empty()) return false;

  // Drop a trailing Eod/EOF if ExpandArgument left one.
  std::size_t n = toks.size();

  while (n > 0 && (toks[n - 1].GetKind() == TokenKind::kEod ||
                   toks[n - 1].GetKind() == TokenKind::kEOF)) {
    --n;
  }

  if (n == 0) return false;

  if (n == 1 && IsStringLiteralKind(toks[0].GetKind())) {
    std::string spelling = CleanTokenSpelling(toks[0]);
    std::string_view raw = spelling;
    // Skip an encoding prefix to find the opening quote.
    std::size_t q = raw.find('"');

    if (q == std::string_view::npos) return false;

    filename = std::string(raw.substr(q + 1, raw.size() - q - 2));
    is_angled = false;

    return true;
  }

  if (n >= 2 && toks[0].GetKind() == TokenKind::kLess &&
      toks[n - 1].GetKind() == TokenKind::kGreater) {
    filename.clear();

    for (std::size_t i = 1; i + 1 < n; ++i) {
      filename += CleanTokenSpelling(toks[i]);
    }

    is_angled = true;

    return true;
  }

  return false;
}

void Preprocessor::HandleIdentDirective(Token& /*ident_tok*/) {
  // #ident / #sccs: silently discard the rest of the line.
  // These are used to embed .comment section strings in ELF, but on non-ELF
  // targets (or when not targeting ELF) Clang discards them silently.
  DiscardUntilEndOfDirective();
}

void Preprocessor::HandlePragmaDirective(Token& pragma_tok) {
  if (callbacks_) callbacks_->PragmaDirective(pragma_tok.GetLocation());

  // A file-scope #pragma breaks the include-guard shape (a #pragma once file is
  // handled by the separate pragma-once mechanism below).
  NoteNonGuardDirectiveAtFileScope();

  Token tok = cur_lexer_->Lex();

  // #pragma once
  if (tok.GetKind() == TokenKind::kIdentifier &&
      LookUpIdentifierInfo(tok)->GetName() == "once") {
    if (header_search_ != nullptr) {
      if (const FileEntry* fe = FileEntryOf(cur_lexer_.get())) {
        header_search_->MarkFileIncludeOnce(fe);
      }
    }
    DiscardUntilEndOfDirective();
    return;
  }

  // Editor-only / no-op pragmas that Clang consumes entirely (no -E output).
  if (tok.GetKind() == TokenKind::kIdentifier) {
    std::string_view ns = LookUpIdentifierInfo(tok)->GetName();

    if (ns == "mark" || ns == "region" || ns == "endregion") {
      DiscardUntilEndOfDirective();
      return;
    }

    if (ns == "message") {
      HandlePragmaMessageLike(pragma_tok, "message", /*is_gcc=*/false);
      return;
    }

    if (ns == "GCC" || ns == "clang") {
      HandleGCCOrClangPragma(pragma_tok, tok);
      return;
    }

    if (ns == "STDC") {
      HandleSTDCPragma(pragma_tok);
      return;
    }

    if (ns == "push_macro") {
      HandlePushMacroPragma();
      return;
    }

    if (ns == "pop_macro") {
      HandlePopMacroPragma();
      return;
    }
  }

  // Unknown pragma with a non-empty body: pass through to the output so it
  // survives -E.
  if (tok.GetKind() != TokenKind::kEod && tok.GetKind() != TokenKind::kEOF) {
    std::vector<Token> out;

    const char* hash_data = nullptr;
    SourceLocation hash_loc = scratch_.GetToken("#", hash_data);
    out.emplace_back(hash_loc, TokenKind::kHash, hash_data, 1u,
                     TokenFlag::kStartOfLine);

    out.push_back(pragma_tok);
    out.push_back(tok);
    for (;;) {
      Token next = cur_lexer_->Lex();
      if (next.GetKind() == TokenKind::kEod ||
          next.GetKind() == TokenKind::kEOF)
        break;
      out.push_back(next);
    }

    EnterTokenStream(std::move(out));
  }
}

void Preprocessor::HandleGCCOrClangPragma(Token& pragma_tok, Token& ns_tok) {
  Token sub = cur_lexer_->Lex();

  if (sub.GetKind() != TokenKind::kIdentifier) {
    DiscardUntilEndOfDirective();
    return;
  }

  std::string_view sub_name = LookUpIdentifierInfo(sub)->GetName();

  // #pragma GCC poison / #pragma clang poison
  if (sub_name == "poison") {
    HandlePragmaPoison(pragma_tok);
    return;
  }

  // #pragma GCC system_header / #pragma clang system_header
  if (sub_name == "system_header") {
    // Promote the current file to a system header from this point on. For a
    // file-backed header this is recorded in HeaderSearch so that later
    // lookups, the InclusionDirective callback, and IsInSystemHeader() all see
    // it; GetCurrentFileCharacteristic() reads the same record.
    if (header_search_ != nullptr) {
      if (const FileEntry* fe = FileEntryOf(cur_lexer_.get())) {
        header_search_->MarkFileAsSystemHeader(fe);
      }
    }

    DiscardUntilEndOfDirective();
    return;
  }

  // #pragma GCC dependency
  if (sub_name == "dependency") {
    HandlePragmaDependency();
    return;
  }

  // #pragma GCC diagnostic / #pragma clang diagnostic: re-emit verbatim so the
  // directive survives -E (Clang prints it in preprocessed output).
  if (sub_name == "diagnostic") {
    std::vector<Token> out;
    const char* hash_data = nullptr;
    SourceLocation hash_loc = scratch_.GetToken("#", hash_data);
    out.emplace_back(hash_loc, TokenKind::kHash, hash_data, 1u,
                     TokenFlag::kStartOfLine);
    const char* pragma_data = nullptr;
    SourceLocation pragma_loc = scratch_.GetToken("pragma", pragma_data);
    Token pragma_t(pragma_loc, TokenKind::kIdentifier, pragma_data, 6u,
                   TokenFlag::kNone);
    LookUpIdentifierInfo(pragma_t);
    out.push_back(pragma_t);
    ns_tok.SetFlag(TokenFlag::kLeadingSpace);
    out.push_back(ns_tok);
    sub.SetFlag(TokenFlag::kLeadingSpace);
    out.push_back(sub);

    for (;;) {
      Token next = cur_lexer_->Lex();

      if (next.GetKind() == TokenKind::kEod ||
          next.GetKind() == TokenKind::kEOF)
        break;
      out.push_back(next);
    }

    EnterTokenStream(std::move(out));
    return;
  }

  // #pragma GCC warning / #pragma GCC error: re-emit with a single string.
  if (sub_name == "warning") {
    HandlePragmaMessageLike(pragma_tok, "warning", /*is_gcc=*/true);
    return;
  }

  if (sub_name == "error") {
    HandlePragmaMessageLike(pragma_tok, "error", /*is_gcc=*/true);
    return;
  }

  // Unknown sub-pragma: consume and ignore.
  DiscardUntilEndOfDirective();
}

void Preprocessor::HandlePragmaMessageLike(const Token& pragma_tok,
                                           std::string_view kind, bool is_gcc) {
  // Read (macro-expanded) the rest of the directive line.
  bool paren_form = false;
  std::string concatenated;
  bool invalid = false;
  bool seen_string = false;
  int paren_depth = 0;

  for (;;) {
    Token t = LexDirectiveToken();

    if (t.GetKind() == TokenKind::kEod || t.GetKind() == TokenKind::kEOF) break;

    if (t.GetKind() == TokenKind::kLParen) {
      if (paren_depth == 0 && !seen_string) paren_form = true;
      ++paren_depth;
      continue;
    }

    if (t.GetKind() == TokenKind::kRParen) {
      if (paren_depth > 0) --paren_depth;
      continue;
    }

    if (IsStringLiteralKind(t.GetKind())) {
      concatenated += DecodeStringLiteralBytes(t.GetLexeme());
      seen_string = true;
      continue;
    }

    invalid = true;
  }

  // Malformed pragma (no string, a stray non-string token, or unbalanced
  // parens): consume quietly, matching Clang (which emits a diagnostic but no
  // -E output).
  if (invalid || !seen_string || paren_depth != 0) {
    return;
  }

  std::string encoded = EncodePragmaMessageBytes(concatenated);
  std::string quoted = "\"" + encoded + "\"";

  // Build the re-emitted token sequence: `#pragma [GCC] <kind> [<string>]`,
  // or `#pragma message(<string>)` when the message form used parens.
  std::vector<Token> out;
  const char* hash_data = nullptr;
  SourceLocation hash_loc = scratch_.GetToken("#", hash_data);
  out.emplace_back(hash_loc, TokenKind::kHash, hash_data, 1u,
                   TokenFlag::kStartOfLine);

  const char* pragma_data = nullptr;
  SourceLocation pragma_loc = scratch_.GetToken("pragma", pragma_data);
  Token pragma_t(pragma_loc, TokenKind::kIdentifier, pragma_data, 6u,
                 TokenFlag::kNone);
  LookUpIdentifierInfo(pragma_t);
  out.push_back(pragma_t);

  if (is_gcc) {
    const char* gcc_data = nullptr;
    SourceLocation gcc_loc = scratch_.GetToken("GCC", gcc_data);
    Token gcc_t(gcc_loc, TokenKind::kIdentifier, gcc_data, 3u,
                TokenFlag::kLeadingSpace);
    LookUpIdentifierInfo(gcc_t);
    out.push_back(gcc_t);
  }

  const char* kind_data = nullptr;
  SourceLocation kind_loc = scratch_.GetToken(kind, kind_data);
  Token kind_t(kind_loc, TokenKind::kIdentifier, kind_data,
               static_cast<uint32_t>(kind.size()), TokenFlag::kLeadingSpace);
  LookUpIdentifierInfo(kind_t);
  out.push_back(kind_t);

  const char* str_data = nullptr;
  SourceLocation str_loc = scratch_.GetToken(quoted, str_data);
  Token str_t(str_loc, TokenKind::kStringLiteral, str_data,
              static_cast<uint32_t>(quoted.size()), TokenFlag::kNone);

  if (!is_gcc) {
    // `#pragma message` always re-emits as `message("<s>")`.
    const char* lp_data = nullptr;
    SourceLocation lp_loc = scratch_.GetToken("(", lp_data);
    out.emplace_back(lp_loc, TokenKind::kLParen, lp_data, 1u, TokenFlag::kNone);
    out.push_back(str_t);
    const char* rp_data = nullptr;
    SourceLocation rp_loc = scratch_.GetToken(")", rp_data);
    out.emplace_back(rp_loc, TokenKind::kRParen, rp_data, 1u, TokenFlag::kNone);
  } else {
    str_t.SetFlag(TokenFlag::kLeadingSpace);
    out.push_back(str_t);
  }

  EnterTokenStream(std::move(out));
}

bool Preprocessor::HandlePragmaOperator(Token& tok) {
  // Must be followed by '(' ... ')'.
  Token open{SourceLocation{}, TokenKind::kUnknown, nullptr, 0u};
  LexUnexpandedToken(open);

  if (open.GetKind() != TokenKind::kLParen) {
    if (open.GetKind() != TokenKind::kEOF &&
        open.GetKind() != TokenKind::kEod) {
      EnterTokenStream(std::vector<Token>{open});
    }
    return false;  // not a _Pragma(...) call: emit _Pragma as an identifier.
  }

  // Collect the parenthesised argument without expanding it, then fully expand
  // it (the operand of _Pragma is macro-expanded, e.g. _Pragma(STRINGIFY(x))).
  std::vector<Token> arg;
  int depth = 1;
  for (;;) {
    Token t{SourceLocation{}, TokenKind::kUnknown, nullptr, 0u};
    LexUnexpandedToken(t);
    if (t.GetKind() == TokenKind::kEOF || t.GetKind() == TokenKind::kEod) break;
    if (t.GetKind() == TokenKind::kLParen) {
      ++depth;
      arg.push_back(t);
      continue;
    }
    if (t.GetKind() == TokenKind::kRParen) {
      --depth;
      if (depth == 0) break;
      arg.push_back(t);
      continue;
    }
    arg.push_back(t);
  }

  std::vector<Token> expanded = ExpandArgument(arg);
  const Token* str = nullptr;
  for (const Token& t : expanded) {
    if (IsStringLiteralKind(t.GetKind())) {
      str = &t;
      break;
    }
  }
  if (str == nullptr) {
    return true;  // malformed: consume silently.
  }

  std::string decoded = DecodeStringLiteralBytes(str->GetLexeme());
  std::string src = "#pragma " + decoded + "\n";

  FileID fid = sm_.CreateFileID("<_Pragma>", src, tok.GetLocation());
  auto pragma_lexer = std::make_unique<PPLexer>(sm_, fid, &diags_);

  std::unique_ptr<PPLexer> saved = std::move(cur_lexer_);
  cur_lexer_ = std::move(pragma_lexer);

  Token hash = cur_lexer_->Lex();

  if (hash.GetKind() == TokenKind::kHash && hash.IsStartOfLine()) {
    HandleDirective(hash);
  }

  // Restore the real file lexer. If the pragma re-emitted tokens, the scratch
  // lexer was pushed onto the include stack — replace it there so the stream
  // resumes into the real file.
  if (cur_token_lexer_) {
    if (!include_macro_stack_.empty()) {
      include_macro_stack_.back().lexer = std::move(saved);
    }
    cur_lexer_.reset();
  } else {
    cur_lexer_ = std::move(saved);
  }
  return true;
}

void Preprocessor::HandlePragmaPoison(Token& /*pragma_tok*/) {
  for (;;) {
    Token tok = cur_lexer_->Lex();
    if (tok.GetKind() == TokenKind::kEod || tok.GetKind() == TokenKind::kEOF)
      break;

    if (tok.GetKind() != TokenKind::kIdentifier) {
      diags_.Report(tok.GetLocation(), diag::err_pp_poison_non_identifier);
      DiscardUntilEndOfDirective();
      return;
    }

    IdentifierInfo* ii = LookUpIdentifierInfo(tok);

    // Warn if the identifier is currently defined as a macro.
    if (ii->HasMacroDefinition()) {
      diags_.Report(tok.GetLocation(), diag::warn_pp_poison_after_define);
    }

    poisoned_identifiers_.insert(ii);
  }
}

void Preprocessor::HandlePragmaDependency() {
  // Parse the filename string literal.
  Token filename_tok = cur_lexer_->Lex();
  if (filename_tok.GetKind() != TokenKind::kStringLiteral) {
    // The Eod token is already pulled in some cases; only report if
    // there actually was a non-Eod token.
    if (filename_tok.GetKind() != TokenKind::kEod &&
        filename_tok.GetKind() != TokenKind::kEOF) {
      diags_.Report(filename_tok.GetLocation(),
                    diag::err_pp_pragma_string_literal)
          << "dependency";
    }
    DiscardUntilEndOfDirective();
    return;
  }

  std::string_view raw = filename_tok.GetLexeme();
  std::string filename;
  if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
    filename = std::string(raw.substr(1, raw.size() - 2));
  }

  // Consume any remaining tokens.
  DiscardUntilEndOfDirective();

  // TODO(gc):
  // For now, we accept the pragma silently since we don't have full
  // file-timestamp comparison wired up.
  (void)filename;
}

void Preprocessor::HandlePragmaDiagnostic() {
  // #pragma GCC diagnostic push / pop / error / warning / ignored / fatal
  Token cmd = cur_lexer_->Lex();

  if (cmd.GetKind() != TokenKind::kIdentifier) {
    diags_.Report(cmd.GetLocation(), diag::err_pp_pragma_diagnostic_invalid);
    DiscardUntilEndOfDirective();
    return;
  }

  std::string_view cmd_name = LookUpIdentifierInfo(cmd)->GetName();

  if (cmd_name == "push") {
    DiscardUntilEndOfDirective();
    return;
  }

  if (cmd_name == "pop") {
    DiscardUntilEndOfDirective();
    return;
  }

  // error|warning|ignored|fatal <option>
  if (cmd_name == "error" || cmd_name == "warning" || cmd_name == "ignored" ||
      cmd_name == "fatal") {
    // Parse the -Wflag string literal.
    Token flag = cur_lexer_->Lex();
    // Silently accept (no diagnostic infrastructure yet).
    DiscardUntilEndOfDirective();
    return;
  }

  diags_.Report(cmd.GetLocation(), diag::err_pp_pragma_diagnostic_invalid);
  DiscardUntilEndOfDirective();
}

void Preprocessor::HandleSTDCPragma(Token& /*pragma_tok*/) {
  // #pragma STDC FENV_ACCESS ON|OFF|DEFAULT
  // #pragma STDC FP_CONTRACT ON|OFF|DEFAULT
  // #pragma STDC CX_LIMITED_RANGE ON|OFF|DEFAULT
  // Silently accept and discard, matching Clang's behavior.
  DiscardUntilEndOfDirective();
}

void Preprocessor::HandlePushMacroPragma() {
  // #pragma push_macro("NAME")  — the syntax may include parens as in
  // `#pragma push_macro("X")` or just `#pragma push_macro "X"`.
  Token tok = cur_lexer_->Lex();

  // Skip optional opening '('.
  if (tok.GetKind() == TokenKind::kLParen) {
    tok = cur_lexer_->Lex();
  }

  if (tok.GetKind() != TokenKind::kStringLiteral) {
    diags_.Report(tok.GetLocation(), diag::err_pp_pragma_push_macro)
        << "push_macro(\"name\")";
    DiscardUntilEndOfDirective();
    return;
  }

  std::string_view raw = tok.GetLexeme();
  std::string name;
  if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
    name = std::string(raw.substr(1, raw.size() - 2));
  }

  // Skip optional closing ')'.
  Token close = cur_lexer_->Lex();
  if (close.GetKind() == TokenKind::kRParen) {
    // Consumed the closing paren; now consume the rest.
    DiscardUntilEndOfDirective();
  } else if (close.GetKind() != TokenKind::kEod &&
             close.GetKind() != TokenKind::kEOF) {
    DiscardUntilEndOfDirective();
  }

  if (name.empty()) return;

  IdentifierInfo& ii = identifiers_.Get(name);
  auto it = macros_.find(&ii);
  MacroDirective* current = (it != macros_.end()) ? it->second : nullptr;

  macro_pragma_stack_[name].push_back(current);
}

void Preprocessor::HandlePopMacroPragma() {
  // #pragma pop_macro("NAME")
  Token tok = cur_lexer_->Lex();

  // Skip optional opening '('.
  if (tok.GetKind() == TokenKind::kLParen) {
    tok = cur_lexer_->Lex();
  }

  if (tok.GetKind() != TokenKind::kStringLiteral) {
    diags_.Report(tok.GetLocation(), diag::err_pp_pragma_pop_macro)
        << "pop_macro(\"name\")";
    DiscardUntilEndOfDirective();
    return;
  }

  std::string_view raw = tok.GetLexeme();
  std::string name;
  if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
    name = std::string(raw.substr(1, raw.size() - 2));
  }

  // Skip optional closing ')'.
  Token close = cur_lexer_->Lex();
  if (close.GetKind() == TokenKind::kRParen) {
    DiscardUntilEndOfDirective();
  } else if (close.GetKind() != TokenKind::kEod &&
             close.GetKind() != TokenKind::kEOF) {
    DiscardUntilEndOfDirective();
  }

  if (name.empty()) return;

  auto stack_it = macro_pragma_stack_.find(name);
  if (stack_it == macro_pragma_stack_.end() || stack_it->second.empty()) {
    return;
  }

  MacroDirective* saved = stack_it->second.back();
  stack_it->second.pop_back();

  IdentifierInfo& ii = identifiers_.Get(name);

  if (saved == nullptr) {
    auto it = macros_.find(&ii);
    if (it != macros_.end()) {
      macros_.erase(it);
    }
    ii.SetHasMacroDefinition(false);
  } else if (saved->IsDefinition()) {
    macros_[&ii] = saved;
    ii.SetHasMacroDefinition(true);
  } else {
    auto it = macros_.find(&ii);
    if (it != macros_.end()) {
      macros_.erase(it);
    }
    ii.SetHasMacroDefinition(false);
  }
}

//===----------------------------------------------------------------------===//
// Line control and user diagnostics (#line, #error, #warning)
//===----------------------------------------------------------------------===//

namespace {

/// Parses a plain decimal digit sequence into \p out. Returns false if \p s is
/// empty, contains a non-digit, or overflows the permitted #line range.
bool ParseLineNumber(std::string_view s, unsigned& out) {
  if (s.empty()) return false;
  unsigned long value = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<unsigned>(c - '0');
    if (value > 2147483647UL) return false;  // C17 6.10.4 upper bound
  }
  out = static_cast<unsigned>(value);
  return true;
}

/// Decodes a string-literal lexeme (including quotes and any escapes) into its
/// character value, for use as a #line filename.
std::string DecodeStringLiteral(std::string_view lexeme) {
  std::string out;
  // Skip any encoding prefix and the opening quote.
  std::size_t i = 0;
  while (i < lexeme.size() && lexeme[i] != '"') ++i;
  ++i;  // past opening quote
  for (; i < lexeme.size() && lexeme[i] != '"'; ++i) {
    if (lexeme[i] == '\\' && i + 1 < lexeme.size()) ++i;  // keep escaped char
    out += lexeme[i];
  }
  return out;
}

}  // namespace

void Preprocessor::HandleLineDirective(Token& line_tok) {
  NoteNonGuardDirectiveAtFileScope();

  // The line number (and optional filename) are macro-expanded (C17 6.10.4).
  Token num = LexDirectiveToken();
  unsigned line_no = 0;
  if (num.GetKind() != TokenKind::kNumericConstant ||
      !ParseLineNumber(num.GetLexeme(), line_no)) {
    diags_.Report(num.GetLocation(), diag::err_pp_line_requires_integer);
    if (num.GetKind() != TokenKind::kEod && num.GetKind() != TokenKind::kEOF) {
      DiscardUntilEndOfDirective();
    }
    return;
  }

  Token next = LexDirectiveToken();
  std::string filename;
  bool have_filename = false;
  if (next.GetKind() == TokenKind::kStringLiteral) {
    filename = DecodeStringLiteral(next.GetLexeme());
    have_filename = true;
    next = LexDirectiveToken();
  } else if (next.GetKind() != TokenKind::kEod &&
             next.GetKind() != TokenKind::kEOF) {
    // A non-string, non-terminator token where the filename belongs is invalid.
    diags_.Report(next.GetLocation(), diag::err_pp_line_invalid_filename);
    DiscardUntilEndOfDirective();
    return;
  }

  // Ignore (but consume) any tokens after a well-formed directive.
  if (next.GetKind() != TokenKind::kEod && next.GetKind() != TokenKind::kEOF) {
    DiscardUntilEndOfDirective();
  }

  // Anchor the override on the directive's own line (always a file location),
  // not the number token, which may have come from a macro or scratch buffer.
  sm_.AddLineDirective(
      line_tok.GetLocation(), line_no,
      have_filename ? std::string_view{filename} : std::string_view{});
}

void Preprocessor::HandleUserDiagnosticDirective(Token& tok, bool is_error) {
  NoteNonGuardDirectiveAtFileScope();

  // The message is the rest of the line, verbatim and unexpanded (C17 6.10.5).
  std::string message;
  bool first = true;

  for (Token t = cur_lexer_->Lex();
       t.GetKind() != TokenKind::kEod && t.GetKind() != TokenKind::kEOF;
       t = cur_lexer_->Lex()) {
    if (!first && t.HasLeadingSpace()) message += ' ';

    message += t.GetLexeme();
    first = false;
  }

  diags_.Report(tok.GetLocation(), is_error ? diag::err_pp_error_directive
                                            : diag::warn_pp_warning_directive)
      << message;
}

//===----------------------------------------------------------------------===//
// Conditional inclusion (#if family)
//===----------------------------------------------------------------------===//

bool Preprocessor::CheckIsMacroDefinedOperand(IdentifierInfo** out_name) {
  Token name = cur_lexer_->Lex();

  if (name.GetKind() != TokenKind::kIdentifier) {
    diags_.Report(name.GetLocation(), diag::err_pp_expected_macro_name);
    // Only consume more if the line has not already ended.
    if (name.GetKind() != TokenKind::kEod &&
        name.GetKind() != TokenKind::kEOF) {
      DiscardUntilEndOfDirective();
    }
    return false;
  }

  IdentifierInfo* ii = LookUpIdentifierInfo(name);
  if (out_name) *out_name = ii;
  bool defined = IsMacroDefined(ii);
  DiscardUntilEndOfDirective();
  return defined;
}

void Preprocessor::HandleIfDirective(Token& if_tok) {
  // A file-scope #if that is not the guard's #ifndef breaks the guard shape.
  if (cur_lexer_->GetConditionalStackDepth() == 0) {
    cur_lexer_->GetMIOpt().EnterOtherTopLevelConditional();
  }
  bool cond = EvaluateDirectiveExpression();  // consumes the rest of the line
  if (callbacks_) callbacks_->If(if_tok.GetLocation(), cond);
  cur_lexer_->PushConditionalLevel(if_tok.GetLocation(), /*was_skipping=*/false,
                                   /*found_non_skip=*/cond,
                                   /*found_else=*/false);
  if (!cond) SkipExcludedConditionalBlock();
}

void Preprocessor::HandleIfdefDirective(Token& tok, bool is_ifndef) {
  bool at_file_scope = cur_lexer_->GetConditionalStackDepth() == 0;
  IdentifierInfo* name = nullptr;
  bool defined =
      CheckIsMacroDefinedOperand(&name);  // consumes rest of the line

  // Include-guard tracking: a file-scope `#ifndef GUARD` is the candidate guard
  // opener; any other file-scope conditional breaks the guard shape.
  if (at_file_scope) {
    if (is_ifndef && name != nullptr) {
      cur_lexer_->GetMIOpt().EnterTopLevelIfndef(name);
    } else {
      cur_lexer_->GetMIOpt().EnterOtherTopLevelConditional();
    }
  }

  if (callbacks_) {
    if (is_ifndef) {
      callbacks_->Ifndef(tok.GetLocation(), name, defined);
    } else {
      callbacks_->Ifdef(tok.GetLocation(), name, defined);
    }
  }

  bool cond = is_ifndef ? !defined : defined;
  cur_lexer_->PushConditionalLevel(tok.GetLocation(), /*was_skipping=*/false,
                                   /*found_non_skip=*/cond,
                                   /*found_else=*/false);
  if (!cond) SkipExcludedConditionalBlock();
}

void Preprocessor::HandleEndifDirective(Token& endif_tok) {
  DiscardUntilEndOfDirective();
  unsigned depth = cur_lexer_->GetConditionalStackDepth();
  PPConditionalInfo info;
  if (cur_lexer_->PopConditionalLevel(info)) {
    diags_.Report(endif_tok.GetLocation(), diag::err_pp_endif_without_if);
    return;
  }
  if (callbacks_) callbacks_->Endif(endif_tok.GetLocation());
  // This #endif closes the outermost conditional: the end of a candidate guard.
  if (depth == 1) cur_lexer_->GetMIOpt().ExitTopLevelConditional();
}

void Preprocessor::HandleElseDirective(Token& else_tok) {
  DiscardUntilEndOfDirective();

  if (cur_lexer_->GetConditionalStackDepth() == 0) {
    diags_.Report(else_tok.GetLocation(), diag::err_pp_without_if) << "else";
    return;
  }

  if (callbacks_) callbacks_->Else(else_tok.GetLocation());

  // A #else on the outermost conditional means the file is not a plain guard.
  if (cur_lexer_->GetConditionalStackDepth() == 1) {
    cur_lexer_->GetMIOpt().Invalidate();
  }

  PPConditionalInfo& top = cur_lexer_->PeekConditionalLevel();
  if (top.found_else) {
    diags_.Report(else_tok.GetLocation(), diag::err_pp_after_else) << "else";
  }
  top.found_else = true;

  // Reaching #else in normal processing means the active branch has ended, so
  // the #else body is skipped down to the #endif.
  SkipExcludedConditionalBlock();
}

void Preprocessor::HandleElifFamilyDirective(Token& tok, PPKeyword kw) {
  if (cur_lexer_->GetConditionalStackDepth() == 0) {
    DiscardUntilEndOfDirective();
    diags_.Report(tok.GetLocation(), diag::err_pp_without_if)
        << ElifKeywordName(kw);
    return;
  }

  // Reaching an #elif in normal processing means a branch was already taken, so
  // this one is not taken; its condition is not evaluated.
  if (callbacks_) callbacks_->Elif(tok.GetLocation(), /*condition=*/false);

  // An #elif on the outermost conditional means the file is not a plain guard.
  if (cur_lexer_->GetConditionalStackDepth() == 1) {
    cur_lexer_->GetMIOpt().Invalidate();
  }

  if (cur_lexer_->PeekConditionalLevel().found_else) {
    diags_.Report(tok.GetLocation(), diag::err_pp_after_else)
        << ElifKeywordName(kw);
  }

  // Reaching an #elif in normal processing means a branch was already taken;
  // skip this one's condition and the remainder down to the #endif.
  DiscardUntilEndOfDirective();
  SkipExcludedConditionalBlock();
}

void Preprocessor::SkipExcludedConditionalBlock() {
  for (;;) {
    Token tok = cur_lexer_->Lex();

    if (tok.GetKind() == TokenKind::kEOF) {
      // Leave the open levels in place; HandleEndOfFile reports the
      // unterminated conditional once, uniformly with the non-skipping case.
      return;
    }

    // Only start-of-line '#' introduces a directive.
    if (tok.GetKind() != TokenKind::kHash || !tok.IsStartOfLine()) continue;

    cur_lexer_->SetParsingPreprocessorDirective(true);
    Token dir = cur_lexer_->Lex();
    if (dir.GetKind() != TokenKind::kIdentifier) {
      DiscardUntilEndOfDirective();
      continue;
    }

    PPKeyword kw = LookUpIdentifierInfo(dir)->GetPPKeyword();

    if (kw == PPKeyword::kIf || kw == PPKeyword::kIfdef ||
        kw == PPKeyword::kIfndef) {
      // A nested conditional is skipped in its entirety.
      cur_lexer_->PushConditionalLevel(dir.GetLocation(), /*was_skipping=*/true,
                                       /*found_non_skip=*/true,
                                       /*found_else=*/false);
      DiscardUntilEndOfDirective();
      continue;
    }

    if (kw == PPKeyword::kEndif) {
      DiscardUntilEndOfDirective();
      PPConditionalInfo info;
      cur_lexer_->PopConditionalLevel(info);
      if (!info.was_skipping) {
        // Closed the group we were skipping. Its opener already fired a
        // callback, so balance it with an Endif; nested dead conditionals fire
        // neither.
        if (callbacks_) callbacks_->Endif(dir.GetLocation());
        return;
      }
      continue;
    }

    // #else / #elif family: act only for the group being skipped (its level
    // was not itself skipped into).
    PPConditionalInfo& top = cur_lexer_->PeekConditionalLevel();
    if (top.was_skipping) {
      DiscardUntilEndOfDirective();
      continue;
    }

    if (kw == PPKeyword::kElse) {
      if (callbacks_) callbacks_->Else(dir.GetLocation());
      if (top.found_else) {
        diags_.Report(dir.GetLocation(), diag::err_pp_after_else) << "else";
      }
      top.found_else = true;
      DiscardUntilEndOfDirective();
      if (!top.found_non_skip) {
        top.found_non_skip = true;
        return;  // the #else branch becomes active
      }
      continue;
    }

    if (kw == PPKeyword::kElif) {
      if (top.found_else || top.found_non_skip) {
        if (callbacks_)
          callbacks_->Elif(dir.GetLocation(), /*condition=*/false);
        DiscardUntilEndOfDirective();
        continue;
      }
      bool cond = EvaluateDirectiveExpression();
      if (callbacks_) callbacks_->Elif(dir.GetLocation(), cond);
      if (cond) {
        top.found_non_skip = true;
        return;  // this #elif branch becomes active
      }
      continue;
    }

    if (kw == PPKeyword::kElifdef || kw == PPKeyword::kElifndef) {
      if (top.found_else || top.found_non_skip) {
        if (callbacks_)
          callbacks_->Elif(dir.GetLocation(), /*condition=*/false);
        DiscardUntilEndOfDirective();
        continue;
      }
      bool defined = CheckIsMacroDefinedOperand();
      bool cond = (kw == PPKeyword::kElifdef) ? defined : !defined;
      if (callbacks_) callbacks_->Elif(dir.GetLocation(), cond);
      if (cond) {
        top.found_non_skip = true;
        return;
      }
      continue;
    }

    // Any other directive is inert while skipping (e.g. a #define in a dead
    // branch must not take effect).
    DiscardUntilEndOfDirective();
  }
}

}  // namespace bcc
