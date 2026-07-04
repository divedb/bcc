#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <vector>

#include "bcc/basic/diagnostic.hh"
#include "bcc/basic/diagnostic_ids.hh"
#include "bcc/basic/source_manager.hh"
#include "bcc/pp/identifier_table.hh"
#include "bcc/lex/lexer.hh"
#include "bcc/pp/macro_args.hh"
#include "bcc/pp/macro_info.hh"
#include "bcc/pp/pp_callbacks.hh"
#include "bcc/pp/preprocessor.hh"
#include "bcc/pp/scratch_buffer.hh"
#include "bcc/lex/token_kind.hh"
#include "bcc/pp/token_lexer.hh"

namespace bcc {

//===----------------------------------------------------------------------===//
// Macro table
//===----------------------------------------------------------------------===//

MacroInfo* Preprocessor::AllocateMacroInfo(SourceLocation loc) {
  macro_infos_.push_back(std::make_unique<MacroInfo>(loc));
  return macro_infos_.back().get();
}

void Preprocessor::AppendDefMacroDirective(IdentifierInfo* ii, MacroInfo* macro,
                                           SourceLocation loc) {
  auto it = macros_.find(ii);
  MacroDirective* previous = (it != macros_.end()) ? it->second : nullptr;

  macro_directives_.push_back(std::make_unique<MacroDirective>(
      MacroDirective::Kind::Define, macro, loc, previous));
  macros_[ii] = macro_directives_.back().get();
  ii->SetHasMacroDefinition(true);

  if (callbacks_) callbacks_->MacroDefined(ii, macro);
}

void Preprocessor::AppendUndefMacroDirective(IdentifierInfo* ii,
                                             SourceLocation loc) {
  auto it = macros_.find(ii);
  MacroDirective* previous = (it != macros_.end()) ? it->second : nullptr;

  macro_directives_.push_back(std::make_unique<MacroDirective>(
      MacroDirective::Kind::Undefine, nullptr, loc, previous));
  macros_[ii] = macro_directives_.back().get();
  ii->SetHasMacroDefinition(false);

  if (callbacks_) callbacks_->MacroUndefined(ii);
}

MacroInfo* Preprocessor::GetMacroInfo(const IdentifierInfo* ii) const {
  if (ii == nullptr || !ii->HasMacroDefinition()) return nullptr;

  auto it = macros_.find(ii);
  if (it == macros_.end()) return nullptr;

  MacroDirective* md = it->second;
  return md->IsDefinition() ? md->GetMacroInfo() : nullptr;
}

void Preprocessor::ForEachDefinedMacro(
    std::function<void(const IdentifierInfo*, const MacroInfo*)> visitor) const {
  for (const auto& [ii, md] : macros_) {
    if (md != nullptr && md->IsDefinition() && md->GetMacroInfo() != nullptr) {
      visitor(ii, md->GetMacroInfo());
    }
  }
}

//===----------------------------------------------------------------------===//
// Builtin macros
//===----------------------------------------------------------------------===//

IdentifierInfo* Preprocessor::RegisterBuiltin(const char* name) {
  IdentifierInfo& ii = identifiers_.Get(name);
  ii.SetIsBuiltinMacro(true);
  MacroInfo* macro = AllocateMacroInfo(SourceLocation{});
  macro->SetIsBuiltinMacro(true);
  AppendDefMacroDirective(&ii, macro, SourceLocation{});
  return &ii;
}

void Preprocessor::RegisterBuiltinMacros() {
  ident_line_           = RegisterBuiltin("__LINE__");
  ident_file_           = RegisterBuiltin("__FILE__");
  ident_date_           = RegisterBuiltin("__DATE__");
  ident_time_           = RegisterBuiltin("__TIME__");
  ident_counter_        = RegisterBuiltin("__COUNTER__");
  ident_include_level_  = RegisterBuiltin("__INCLUDE_LEVEL__");
  ident_stdc_           = RegisterBuiltin("__STDC__");
  ident_stdc_hosted_    = RegisterBuiltin("__STDC_HOSTED__");
  ident_stdc_version_   = RegisterBuiltin("__STDC_VERSION__");
  ident_base_file_      = RegisterBuiltin("__BASE_FILE__");
  ident_file_name_      = RegisterBuiltin("__FILE_NAME__");
  ident_timestamp_      = RegisterBuiltin("__TIMESTAMP__");
  ident_flt_eval_method_ = RegisterBuiltin("__FLT_EVAL_METHOD__");
  ident_pragma_         = RegisterBuiltin("_Pragma");
  ident_bitint_maxwidth_ = RegisterBuiltin("__BITINT_MAXWIDTH__");
  ident_char16_type_    = RegisterBuiltin("__CHAR16_TYPE__");
  ident_char32_type_    = RegisterBuiltin("__CHAR32_TYPE__");
  ident_wchar_max_      = RegisterBuiltin("__WCHAR_MAX__");
}

namespace {

/// Wraps \p s as a C string literal, escaping backslashes and double quotes so
/// paths such as C:\foo produce a valid token.
std::string MakeStringLiteral(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  out += "\"";
  return out;
}

}  // namespace

void Preprocessor::ExpandBuiltinMacro(Token& tok) {
  IdentifierInfo* ii = tok.GetIdentifierInfo();

  std::string spelling;
  TokenKind kind = TokenKind::kNumericConstant;

  if (ii == ident_line_) {
    // Use the expansion (use-site) location so __LINE__ inside a macro reports
    // where the macro was invoked.
    PresumedLoc pl = sm_.GetPresumedLoc(sm_.GetExpansionLoc(tok.GetLocation()));
    spelling = std::to_string(pl.IsValid() ? pl.line : 0u);
  } else if (ii == ident_file_) {
    kind = TokenKind::kStringLiteral;
    PresumedLoc pl = sm_.GetPresumedLoc(sm_.GetExpansionLoc(tok.GetLocation()));
    spelling = MakeStringLiteral(pl.IsValid() ? pl.filename : std::string_view{});
  } else if (ii == ident_counter_) {
    spelling = std::to_string(counter_++);
  } else if (ii == ident_include_level_) {
    unsigned level = 0;
    for (const IncludeStackInfo& e : include_macro_stack_) {
      if (e.lexer != nullptr) ++level;  // count suspended file lexers only
    }
    spelling = std::to_string(level);
  } else if (ii == ident_date_) {
    kind = TokenKind::kStringLiteral;
    spelling = date_literal_;
  } else if (ii == ident_time_) {
    kind = TokenKind::kStringLiteral;
    spelling = time_literal_;
  } else if (ii == ident_stdc_) {
    spelling = "1";
  } else if (ii == ident_stdc_hosted_) {
    spelling = "1";
  } else if (ii == ident_stdc_version_) {
    spelling = "201112L";
  } else if (ii == ident_base_file_) {
    kind = TokenKind::kStringLiteral;
    spelling = MakeStringLiteral(base_file_name_);
  } else if (ii == ident_file_name_) {
    kind = TokenKind::kStringLiteral;
    PresumedLoc pl = sm_.GetPresumedLoc(sm_.GetExpansionLoc(tok.GetLocation()));
    std::string_view full = pl.IsValid() ? std::string_view{pl.filename}
                                         : std::string_view{};
    std::string_view::size_type slash = full.find_last_of('/');
    std::string_view basename =
        (slash == std::string_view::npos) ? full : full.substr(slash + 1);
    spelling = MakeStringLiteral(basename);
  } else if (ii == ident_timestamp_) {
    kind = TokenKind::kStringLiteral;
    PresumedLoc pl = sm_.GetPresumedLoc(sm_.GetExpansionLoc(tok.GetLocation()));
    if (pl.IsValid()) {
      struct stat st;
      std::string fn(pl.filename);
      if (stat(fn.c_str(), &st) == 0) {
        char buf[64];
        struct tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &st.st_mtime);
#else
        localtime_r(&st.st_mtime, &tm);
#endif
        std::strftime(buf, sizeof(buf), "%a %b %e %T %Y", &tm);
        spelling = MakeStringLiteral(buf);
      } else {
        spelling = MakeStringLiteral("??? ??? ?? ??:??:?? ????");
      }
    } else {
      spelling = MakeStringLiteral("??? ??? ?? ??:??:?? ????");
    }
  } else if (ii == ident_flt_eval_method_) {
    spelling = "0";
  } else if (ii == ident_bitint_maxwidth_) {
    spelling = "8388608";
  } else if (ii == ident_char16_type_) {
    kind = TokenKind::kIdentifier;
    spelling = "unsigned short";
  } else if (ii == ident_char32_type_) {
    kind = TokenKind::kIdentifier;
    spelling = "unsigned int";
  } else if (ii == ident_wchar_max_) {
    spelling = "0x7fffffff";
  } else if (ii == ident_pragma_) {
    return;
  } else {
    return;
  }

  const char* data = nullptr;
  SourceLocation loc = scratch_.GetToken(spelling, data);

  TokenFlag flags = TokenFlag::kNone;
  if (tok.IsStartOfLine()) flags |= TokenFlag::kStartOfLine;
  if (tok.HasLeadingSpace()) flags |= TokenFlag::kLeadingSpace;

  tok = Token(loc, kind, data, static_cast<uint32_t>(spelling.size()), flags);
}

//===----------------------------------------------------------------------===//
// Expansion entry points
//===----------------------------------------------------------------------===//

bool Preprocessor::HandleIdentifier(Token& tok) {
  if (tok.GetIdentifierInfo() == nullptr) {
    LookUpIdentifierInfo(tok);
  }
  // Check for poisoned identifiers before attempting macro expansion.
  IdentifierInfo* ii = tok.GetIdentifierInfo();
  if (ii != nullptr && poisoned_identifiers_.count(ii) > 0) {
    diags_.Report(tok.GetLocation(), diag::err_pp_poisoned_macro) << ii->GetName();
    // Do not expand; the token is still produced.
    return false;
  }

  // _Pragma("...") operator: evaluate wherever it appears.
  if (ii == ident_pragma_) {
    if (HandlePragmaOperator(tok)) {
      return true;  // consumed; re-dispatch for any re-emitted tokens.
    }
    // Not a call: fall through and emit _Pragma as a plain identifier.
  }

  // __has_builtin / __has_attribute / __has_feature / __has_extension /
  // __is_identifier are operator-like builtins that evaluate wherever they
  // appear (not only inside #if), matching Clang.
  if (ii != nullptr) {
    PPKeyword kw = ii->GetPPKeyword();
    if (kw == PPKeyword::kHasBuiltin || kw == PPKeyword::kHasAttribute ||
        kw == PPKeyword::kHasFeature || kw == PPKeyword::kHasExtension ||
        kw == PPKeyword::kIsIdentifier) {
      Token next{SourceLocation{}, TokenKind::kUnknown, nullptr, 0u};
      LexUnexpandedToken(next);
      bool is_call = (next.GetKind() == TokenKind::kLParen);
      // Push the peeked token back so the evaluator (or the normal path) sees
      // it. An EOF/Eod sentinel is idempotent and need not be requeued.
      if (next.GetKind() != TokenKind::kEOF && next.GetKind() != TokenKind::kEod) {
        EnterTokenStream(std::vector<Token>{next});
      }
      if (is_call) {
        if (kw == PPKeyword::kIsIdentifier) {
          tok = EvaluateIsIdentifier(tok);
        } else {
          tok = EvaluateHasExpression(tok);
        }
        return false;  // tok rewritten in place; emit it.
      }
      // Not a call: fall through to ordinary handling.
    }
  }

  return TryExpandMacro(tok);
}

bool Preprocessor::TryExpandMacro(Token& tok) {
  IdentifierInfo* ii = tok.GetIdentifierInfo();
  if (ii == nullptr || !ii->HasMacroDefinition()) return false;

  if (tok.IsDisableExpand()) return false;

  MacroInfo* macro = GetMacroInfo(ii);
  if (macro == nullptr) return false;

  if (macro->IsBuiltinMacro()) {
    // Builtins produce a single token computed on the fly; rewrite in place and
    // let the caller emit it directly (no token-lexer, no rescanning).
    if (callbacks_) callbacks_->MacroExpands(tok, macro);
    ExpandBuiltinMacro(tok);
    return false;
  }

  if (!macro->IsEnabled()) {
    // Mid-expansion: paint this occurrence so it is never expanded again.
    tok.SetFlag(TokenFlag::kDisableExpand);
    return false;
  }

  if (macro->IsFunctionLike()) {
    // A function-like macro is only expanded when followed by '('.
    if (!IsNextTokenLParen()) return false;

    MacroArgs args = ReadFunctionLikeMacroArgs(macro);
    macro->SetIsUsed(true);
    if (callbacks_) callbacks_->MacroExpands(tok, macro);
    EnterMacro(tok, macro, &args);
    return true;
  }

  macro->SetIsUsed(true);
  if (callbacks_) callbacks_->MacroExpands(tok, macro);
  EnterMacro(tok, macro, nullptr);
  return true;
}

void Preprocessor::EnterMacro(Token& name_tok, MacroInfo* macro,
                              MacroArgs* args) {
  EnterTokenStream(std::make_unique<TokenLexer>(name_tok, macro, args, *this));
}

bool Preprocessor::IsNextTokenLParen() {
  Token tok{SourceLocation{}, TokenKind::kUnknown, nullptr, 0u};
  LexUnexpandedToken(tok);

  if (tok.GetKind() == TokenKind::kLParen) return true;

  // Not a call: push the peeked token back so it is produced next. Nothing to
  // unget for an EOF / end-of-directive (the underlying lexer is idempotent).
  if (tok.GetKind() != TokenKind::kEOF && tok.GetKind() != TokenKind::kEod) {
    EnterTokenStream(std::vector<Token>{tok});
  }
  return false;
}

MacroArgs Preprocessor::ReadFunctionLikeMacroArgs(MacroInfo* macro) {
  // The opening '(' has already been consumed by IsNextTokenLParen().
  // We set paren_depth = 1 to account for that consumed '(' so that a ')'
  // inside an argument that returns to the outer-most level (e.g. the ')'
  // in `(typeof(x))(8)`) is not mistaken for the macro's closing ')'.
  const unsigned num_params = macro->GetNumParams();
  const bool variadic = macro->IsVariadic();
  const unsigned num_named = variadic ? num_params - 1 : num_params;

  std::vector<std::vector<Token>> groups;
  std::vector<Token> current;
  int paren_depth = 1;

  for (;;) {
    Token tok{SourceLocation{}, TokenKind::kUnknown, nullptr, 0u};
    LexUnexpandedToken(tok);

    TokenKind kind = tok.GetKind();
    if (kind == TokenKind::kEOF || kind == TokenKind::kEod) {
      groups.push_back(std::move(current));  // unterminated invocation
      break;
    }

    if (kind == TokenKind::kLParen) {
      ++paren_depth;
      current.push_back(tok);
      continue;
    }
    if (kind == TokenKind::kRParen) {
      if (paren_depth == 1) {
        groups.push_back(std::move(current));
        break;
      }
      --paren_depth;
      current.push_back(tok);
      continue;
    }
    if (kind == TokenKind::kComma && paren_depth == 1) {
      // Once the named arguments are collected, commas belong to __VA_ARGS__.
      if (variadic && groups.size() == num_named) {
        current.push_back(tok);
      } else {
        groups.push_back(std::move(current));
        current.clear();
      }
      continue;
    }

    // Attach IdentifierInfo now so the token can expand during pre-expansion.
    if (kind == TokenKind::kIdentifier && tok.GetIdentifierInfo() == nullptr) {
      LookUpIdentifierInfo(tok);
    }
    current.push_back(tok);
  }

  // Distribute the collected comma-groups across the formal parameters.
  std::vector<std::vector<Token>> args(num_params);
  if (num_params == 0) {
    // F(): the single (possibly empty) group is discarded.
  } else if (!variadic) {
    for (unsigned i = 0; i < num_params && i < groups.size(); ++i) {
      args[i] = std::move(groups[i]);
    }
  } else {
    for (unsigned i = 0; i < num_named && i < groups.size(); ++i) {
      args[i] = std::move(groups[i]);
    }
    if (groups.size() > num_named) {
      args[num_params - 1] = std::move(groups[num_named]);
    }
  }

  return MacroArgs(std::move(args));
}

std::vector<Token> Preprocessor::ExpandArgument(
    const std::vector<Token>& arg_tokens) {
  std::vector<Token> result;
  if (arg_tokens.empty()) return result;

  // Append an EOF sentinel that bounds this stream. If the argument's last token
  // is a function-like macro name, the '(' look-ahead (IsNextTokenLParen) must
  // not scan past the argument into the caller's input: it should see
  // end-of-argument and leave the macro unexpanded, matching gcc/clang. Without
  // the sentinel the look-ahead pops this exhausted lexer and pulls tokens from
  // the enclosing stream (e.g. the next source line) into the expanded argument.
  std::vector<Token> stream = arg_tokens;
  stream.push_back(Token{SourceLocation{}, TokenKind::kEOF, nullptr, 0u});

  const std::size_t base_depth = include_macro_stack_.size();
  EnterTokenStream(std::move(stream));  // pushes one level

  // Drive the dispatch until the argument stream (and everything it spawned)
  // has been popped back to the depth we started at.
  while (include_macro_stack_.size() > base_depth) {
    Token tok{SourceLocation{}, TokenKind::kUnknown, nullptr, 0u};
    if (cur_lexer_callback_(*this, tok)) {
      result.push_back(tok);
    }
  }

  // Drop the sentinel if it was replayed as an ordinary token (no trailing
  // function-like macro consumed it during look-ahead).
  if (!result.empty() && result.back().GetKind() == TokenKind::kEOF) {
    result.pop_back();
  }
  return result;
}

//===----------------------------------------------------------------------===//
// TokenLexer
//===----------------------------------------------------------------------===//

namespace {

/// One-past-end location of the macro-name token in the caller. Byte arithmetic
/// only makes sense for file locations; for a name that itself came from a
/// macro expansion (token-index space) we fall back to a zero-length range.
SourceLocation ComputeInvocationEnd(SourceManager& sm, const Token& name) {
  SourceLocation start = name.GetLocation();
  if (start.IsMacroExpansion()) return start;

  auto [fid, offset] = sm.GetDecomposedLoc(start);
  return sm.GetLocForOffset(
      fid, offset + static_cast<uint32_t>(name.GetLexeme().size()));
}

}  // namespace

TokenLexer::TokenLexer(const Token& macro_name_tok, MacroInfo* macro,
                       MacroArgs* args, Preprocessor& pp)
    : pp_(pp), macro_(macro) {
  first_token_at_start_of_line_ = macro_name_tok.IsStartOfLine();
  first_token_has_leading_space_ = macro_name_tok.HasLeadingSpace();

  if (macro_->IsObjectLike() && !macro_->HasPasteOperator()) {
    // Fast path: replay the replacement list straight from the definition.
    tokens_ = macro_->GetReplacementTokens().data();
    num_tokens_ = macro_->GetNumTokens();
  } else {
    BuildExpansion(args);
    tokens_ = owned_tokens_.data();
    num_tokens_ = static_cast<unsigned>(owned_tokens_.size());
  }

  SourceManager& sm = pp_.GetSourceManager();
  SourceLocation spelling = num_tokens_ > 0 ? tokens_[0].GetLocation()
                                            : macro_->GetDefinitionLoc();
  SourceLocation expansion_start = macro_name_tok.GetLocation();
  SourceLocation expansion_end = ComputeInvocationEnd(sm, macro_name_tok);
  expansion_fid_ =
      sm.CreateExpansionLoc(spelling, expansion_start, expansion_end,
                            num_tokens_ > 0 ? num_tokens_ : 1);

  // Disable only after arguments have been pre-expanded (which happens inside
  // BuildExpansion, in the caller's macro context).
  macro_->DisableExpansion();
}

TokenLexer::TokenLexer(std::vector<Token> tokens, Preprocessor& pp)
    : pp_(pp), macro_(nullptr), owned_tokens_(std::move(tokens)) {
  tokens_ = owned_tokens_.data();
  num_tokens_ = static_cast<unsigned>(owned_tokens_.size());
  is_stream_ = true;
}

TokenLexer::~TokenLexer() {
  if (macro_ != nullptr) macro_->EnableExpansion();
}

bool TokenLexer::Lex(Token& result) {
  if (cur_token_ >= num_tokens_) return false;

  Token tok = tokens_[cur_token_];

  if (!is_stream_) {
    tok.SetLocation(
        pp_.GetSourceManager().GetLocForOffset(expansion_fid_, cur_token_));
    if (cur_token_ == 0) {
      // The first expanded token adopts the invocation's spacing.
      if (first_token_at_start_of_line_) {
        tok.SetFlag(TokenFlag::kStartOfLine);
      } else {
        tok.ClearFlag(TokenFlag::kStartOfLine);
      }
      if (first_token_has_leading_space_) {
        tok.SetFlag(TokenFlag::kLeadingSpace);
      } else {
        tok.ClearFlag(TokenFlag::kLeadingSpace);
      }
    } else if (inherit_leading_space_) {
      // A nested expansion that had a leading space expanded to nothing:
      // that space flows onto this token.
      tok.SetFlag(TokenFlag::kLeadingSpace);
    }
  } else if (inherit_leading_space_) {
    tok.SetFlag(TokenFlag::kLeadingSpace);
  }
  if (inherit_start_of_line_) {
    tok.SetFlag(TokenFlag::kStartOfLine);
  }
  inherit_leading_space_ = false;
  inherit_start_of_line_ = false;

  ++cur_token_;
  result = tok;
  return true;
}

int TokenLexer::ParameterIndex(const Token& tok) const {
  if (!macro_->IsFunctionLike()) return -1;
  IdentifierInfo* ii = tok.GetIdentifierInfo();
  if (ii == nullptr) return -1;
  return macro_->GetParameterIndex(ii);
}

void TokenLexer::BuildExpansion(MacroArgs* args) {
  const std::vector<Token>& body = macro_->GetReplacementTokens();
  bool pending_paste = false;

  for (std::size_t i = 0; i < body.size(); ++i) {
    const Token& cur = body[i];

    // Stringize: '#' followed by a parameter (function-like macros only).
    if (macro_->IsFunctionLike() && cur.GetKind() == TokenKind::kHash &&
        args != nullptr && i + 1 < body.size()) {
      int pidx = ParameterIndex(body[i + 1]);
      if (pidx >= 0) {
        Token s = StringifyArgument(args->GetUnexpArgument(pidx));
        // The stringized result inherits the '#' token's whitespace flags so
        // spacing around the operator is preserved (e.g. `, #name`).
        if (cur.HasLeadingSpace()) {
          s.SetFlag(TokenFlag::kLeadingSpace);
        }
        AppendOrPaste(owned_tokens_, std::vector<Token>{s}, pending_paste);
        pending_paste = false;
        ++i;  // consume the parameter
        continue;
      }
    }

    // Paste operator between two operands. The left operand may be an empty
    // (placemarker) argument — in that case owned_tokens_ has nothing appended
    // for it, but "##" is still the paste operator and must be consumed so the
    // right operand is handled by AppendOrPaste's placemarker logic rather than
    // emitted literally.
    if (cur.GetKind() == TokenKind::kHashHash && i + 1 < body.size()) {
      pending_paste = true;
      continue;
    }

    std::vector<Token> produced;
    int pidx = ParameterIndex(cur);

    // GNU ", ## __VA_ARGS__" comma-elision extension: when the right operand of
    // a paste is the variadic parameter and the variadic argument is empty, the
    // comma preceding the "##" is deleted. This diverges from the ISO
    // placemarker rule (which would keep the comma) and matches gcc/clang. The
    // Linux kernel relies on it in macros like COUNT_ARGS() and IS_ENABLED().
    if (pending_paste && pidx >= 0 && args != nullptr && macro_->IsVariadic() &&
        static_cast<unsigned>(pidx) == macro_->GetNumParams() - 1 &&
        args->GetUnexpArgument(pidx).empty() && !owned_tokens_.empty() &&
        owned_tokens_.back().GetKind() == TokenKind::kComma) {
      owned_tokens_.pop_back();  // drop the comma that precedes "##"
      pending_paste = false;
      continue;
    }

    // __VA_OPT__(content) — C23 / C++20 conditional macro expansion.
    //
    // When the variadic argument is non-empty, the content is substituted
    // (with parameter references expanded); when empty, the content, any
    // pending paste (## on the left), and any immediately following ## (on
    // the right) are all deleted.
    if (macro_->IsFunctionLike() && macro_->IsVariadic() && args != nullptr &&
        cur.GetKind() == TokenKind::kIdentifier &&
        cur.GetIdentifierInfo() != nullptr &&
        cur.GetIdentifierInfo()->GetPPKeyword() == PPKeyword::kVAOpt) {

      // Must be followed by '('.
      if (i + 1 < body.size() && body[i + 1].GetKind() == TokenKind::kLParen) {
        std::vector<Token> va_content;
        std::size_t j = i + 2;  // skip __VA_OPT__ and '('
        int depth = 1;
        while (j < body.size() && depth > 0) {
          if (body[j].GetKind() == TokenKind::kLParen) {
            ++depth;
          } else if (body[j].GetKind() == TokenKind::kRParen) {
            --depth;
            if (depth == 0) break;  // closing paren of __VA_OPT__
          }
          va_content.push_back(body[j]);
          ++j;
        }
        // j now points to the matching ')'.

        unsigned variadic_idx = macro_->GetNumParams() - 1;
        bool va_nonempty = !args->GetUnexpArgument(variadic_idx).empty();

        if (va_nonempty) {
          // Process content with parameter substitution.
          std::vector<Token> produced;
          for (const Token& ct : va_content) {
            int cpidx = ParameterIndex(ct);
            if (cpidx >= 0) {
              const auto& arg_tokens = args->GetUnexpArgument(cpidx);
              for (std::size_t k = 0; k < arg_tokens.size(); ++k) {
                Token t = arg_tokens[k];
                if (k == 0) {
                  if (ct.IsStartOfLine())
                    t.SetFlag(TokenFlag::kStartOfLine);
                  else
                    t.ClearFlag(TokenFlag::kStartOfLine);
                  if (ct.HasLeadingSpace())
                    t.SetFlag(TokenFlag::kLeadingSpace);
                  else
                    t.ClearFlag(TokenFlag::kLeadingSpace);
                }
                produced.push_back(std::move(t));
              }
            } else {
              produced.push_back(ct);
            }
          }
          // The first produced token inherits spacing from __VA_OPT__,
          // overriding any earlier spacing, but still gains a pending
          // leading space if an earlier empty parameter carried one.
          if (!produced.empty()) {
            if (cur.IsStartOfLine())
              produced[0].SetFlag(TokenFlag::kStartOfLine);
            else
              produced[0].ClearFlag(TokenFlag::kStartOfLine);
            if (cur.HasLeadingSpace() || pending_leading_space_)
              produced[0].SetFlag(TokenFlag::kLeadingSpace);
            else
              produced[0].ClearFlag(TokenFlag::kLeadingSpace);
            pending_leading_space_ = false;
          }
          AppendOrPaste(owned_tokens_, produced, pending_paste);
          pending_paste = false;
        } else {
          // Variadic argument is empty: delete __VA_OPT__(content).
          // Any pending paste (## on the left) is also deleted.
          pending_paste = false;
          // If the next token after ')' is ##, delete it too.
          if (j + 1 < body.size() &&
              body[j + 1].GetKind() == TokenKind::kHashHash) {
            ++j;
          }
        }

        i = j;  // advance past ')'; for-loop ++i moves past it
        continue;
      }
      // No '(' after __VA_OPT__: fall through to ordinary token emission.
    }

    bool adjacent_paste = false;
    if (pidx >= 0 && args != nullptr) {
      adjacent_paste =
          pending_paste ||
          (i + 1 < body.size() && body[i + 1].GetKind() == TokenKind::kHashHash);
      produced = adjacent_paste ? args->GetUnexpArgument(pidx)
                                : args->GetExpandedArgument(pidx, pp_);
      // When a parameter with leading space expands to nothing, remember the
      // spacing so the next emitted token inherits it (prevents gluing). A
      // parameter that is an operand of "##" is excluded: an empty (placemarker)
      // paste operand does not contribute a propagating space.
      if (produced.empty() && cur.HasLeadingSpace() && !adjacent_paste) {
        pending_leading_space_ = true;
      }
    } else {
      produced = std::vector<Token>{cur};
    }

    // When a parameter is substituted, the first token of the argument must
    // inherit the whitespace flags from the parameter position in the macro
    // body, not from the call site.  This ensures that macro call-site spacing
    // does not leak into the expansion (e.g. `DECLARE_BITMAP(name, bits)`
    // should not introduce a space before `bits` just because the call had
    // `, 128`).  This also applies when the parameter is the *left* operand of
    // a `##`: its first token becomes the LHS of the paste, and PasteTokens
    // copies the LHS leading-space onto the result, so dropping it here would
    // glue the argument onto the preceding token (e.g. the macro body
    // `struct type##_fract` must emit `struct s8_fract`, not `structs8_fract`).
    // Only the *right* operand of a `##` (pending_paste) is exempt, since its
    // leading space is discarded when it is pasted onto the previous token.
    if (pidx >= 0 && !produced.empty() && !pending_paste) {
      if (cur.IsStartOfLine())
        produced[0].SetFlag(TokenFlag::kStartOfLine);
      else
        produced[0].ClearFlag(TokenFlag::kStartOfLine);
      if (cur.HasLeadingSpace())
        produced[0].SetFlag(TokenFlag::kLeadingSpace);
      else
        produced[0].ClearFlag(TokenFlag::kLeadingSpace);
    }

    // Transfer pending leading space from an empty-parameter substitution
    // to the next set of produced tokens.
    if (pending_leading_space_ && !produced.empty()) {
      produced[0].SetFlag(TokenFlag::kLeadingSpace);
      pending_leading_space_ = false;
    }

    AppendOrPaste(owned_tokens_, produced, pending_paste);
    pending_paste = false;
  }
}

void TokenLexer::AppendOrPaste(std::vector<Token>& out,
                               const std::vector<Token>& produced,
                               bool pending_paste) {
  if (!pending_paste) {
    out.insert(out.end(), produced.begin(), produced.end());
    return;
  }

  // Placemarkers: an empty operand leaves the other side unchanged.
  if (produced.empty()) return;
  if (out.empty()) {
    out.insert(out.end(), produced.begin(), produced.end());
    return;
  }

  std::optional<Token> pasted = PasteTokens(out.back(), produced.front());
  if (pasted.has_value()) {
    out.back() = *pasted;
  } else {
    // Invalid paste: keep both tokens side by side.
    out.push_back(produced.front());
  }
  for (std::size_t k = 1; k < produced.size(); ++k) {
    out.push_back(produced[k]);
  }
}

std::optional<Token> TokenLexer::PasteTokens(const Token& lhs,
                                             const Token& rhs) {
  std::string text;
  text.reserve(lhs.GetLexeme().size() + rhs.GetLexeme().size());
  text += lhs.GetLexeme();
  text += rhs.GetLexeme();

  const char* data = nullptr;
  SourceLocation loc = pp_.GetScratchBuffer().GetToken(text, data);

  // Re-lex the concatenation; it must form exactly one token.
  SourceManager& sm = pp_.GetSourceManager();
  BufferedLexer lexer(sm, sm.GetFileID(loc));
  Token first = lexer.NextToken();
  Token second = lexer.NextToken();

  if (second.GetKind() != TokenKind::kEOF ||
      first.GetLexeme().size() != text.size()) {
    return std::nullopt;
  }

  if (first.GetKind() == TokenKind::kIdentifier) {
    pp_.LookUpIdentifierInfo(first);
  }

  // Preserve leading-spacing and start-of-line flags from the LHS token so that
  // the pasted result reads naturally in the output stream (e.g. the space
  // before `test_` in `int test_##name` remains before `test_dirty`).
  if (lhs.IsStartOfLine())
    first.SetFlag(TokenFlag::kStartOfLine);
  else
    first.ClearFlag(TokenFlag::kStartOfLine);
  if (lhs.HasLeadingSpace())
    first.SetFlag(TokenFlag::kLeadingSpace);
  else
    first.ClearFlag(TokenFlag::kLeadingSpace);
  return first;
}

Token TokenLexer::StringifyArgument(const std::vector<Token>& arg_tokens) {
  std::string text = "\"";
  for (std::size_t i = 0; i < arg_tokens.size(); ++i) {
    const Token& t = arg_tokens[i];

    // Whitespace between argument tokens becomes a single space.
    // The first token never gets a leading space even if flagged.
    if (i > 0) {
      bool spaced = t.HasLeadingSpace() || t.IsStartOfLine();
      if (spaced) text += ' ';
    }

    std::string_view lex = t.GetLexeme();
    if (IsStringLiteralKind(t.GetKind()) || IsCharLiteralKind(t.GetKind())) {
      // Backslashes and quotes inside string/char literals are escaped.
      for (char c : lex) {
        if (c == '"' || c == '\\') text += '\\';
        text += c;
      }
    } else {
      text += lex;
    }
  }
  text += "\"";

  const char* data = nullptr;
  SourceLocation loc = pp_.GetScratchBuffer().GetToken(text, data);
  return Token(loc, TokenKind::kStringLiteral, data,
               static_cast<uint32_t>(text.size()));
}

}  // namespace bcc
