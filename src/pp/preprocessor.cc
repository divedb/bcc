#include "bcc/pp/preprocessor.hh"

#include <cassert>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>
#include <utility>

#include "bcc/basic/diagnostic.hh"
#include "bcc/basic/diagnostic_ids.hh"
#include "bcc/basic/source_manager.hh"
#include "bcc/common/string_util.hh"
#include "bcc/pp/header_search.hh"
#include "bcc/pp/macro_info.hh"
#include "bcc/pp/pp_callbacks.hh"
#include "bcc/pp/token_lexer.hh"

namespace bcc {

namespace {

/// Builds the __DATE__ ("Mmm dd yyyy") and __TIME__ ("hh:mm:ss") string
/// literals, including surrounding quotes, from the current local time.
void InitDateTimeLiterals(std::string& date_out, std::string& time_out) {
  static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr",
                                        "May", "Jun", "Jul", "Aug",
                                        "Sep", "Oct", "Nov", "Dec"};
  std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif

  char buf[32];
  // C requires the day to be space-padded (e.g. "Jul  2 2026").
  std::snprintf(buf, sizeof(buf), "\"%s %2d %d\"", kMonths[tm.tm_mon],
                tm.tm_mday, 1900 + tm.tm_year);
  date_out = buf;

  std::snprintf(buf, sizeof(buf), "\"%02d:%02d:%02d\"", tm.tm_hour, tm.tm_min,
                tm.tm_sec);
  time_out = buf;
}

}  // namespace

Preprocessor::Preprocessor(SourceManager& sm, DiagnosticsEngine& diags,
                           HeaderSearch& header_search)
    : sm_(sm), diags_(diags), header_search_(header_search), scratch_(sm) {
  RegisterBuiltinMacros();
}

Preprocessor::~Preprocessor() = default;

void Preprocessor::SetPPCallbacks(
    std::unique_ptr<PPCallbacks> callbacks) noexcept {
  callbacks_ = std::move(callbacks);
}

void Preprocessor::EnterMainFile() { EnterSourceFile(sm_.GetMainFileID()); }

void Preprocessor::EnsureDateTimeTokens() {
  if (date_loc_.IsValid()) return;

  std::string date;
  std::string time;
  InitDateTimeLiterals(date, time);

  const char* data = nullptr;
  date_loc_ = scratch_.GetToken(date, data);
  time_loc_ = scratch_.GetToken(time, data);
}

void Preprocessor::EnterSourceFile(FileID fid, SourceLocation /*include_loc*/) {
  EnterSourceFileWithLexer(std::make_unique<PPLexer>(sm_, fid, &diags_));
}

void Preprocessor::EnterIncludeFile(FileID fid,
                                    SourceLocation /*include_loc*/) {
  EnterSourceFileWithLexer(std::make_unique<PPLexer>(sm_, fid, &diags_));
}

void Preprocessor::EnterSourceFileWithLexer(std::unique_ptr<PPLexer> lexer) {
  FileID prev_fid = cur_lexer_ ? cur_lexer_->GetFileID() : FileID{};

  if (cur_lexer_) PushIncludeMacroStack();

  cur_lexer_ = std::move(lexer);
  cur_lexer_callback_ = &CLK_Lexer;

  if (callbacks_) {
    callbacks_->FileChanged(sm_.GetLocForStartOfFile(cur_lexer_->GetFileID()),
                            PPCallbacks::FileChangeReason::kEnterFile, prev_fid,
                            GetCurrentFileCharacteristic());
  }
}

void Preprocessor::EnterTokenStream(std::unique_ptr<TokenLexer> token_lexer) {
  PushIncludeMacroStack();
  cur_token_lexer_ = std::move(token_lexer);
  cur_lexer_callback_ = &CLK_TokenLexer;
}

void Preprocessor::EnterTokenStream(std::vector<Token> tokens) {
  EnterTokenStream(std::make_unique<TokenLexer>(std::move(tokens), *this));
}

void Preprocessor::LexUnexpandedToken(Token& result) {
  // Read the next token straight from the lexer stack, crossing exhausted
  // lexers, without macro expansion or directive handling.
  for (;;) {
    if (cur_token_lexer_) {
      if (cur_token_lexer_->Lex(result)) return;
      RemoveTopOfLexerStack();
      continue;
    }

    if (cur_lexer_) {
      result = cur_lexer_->Lex();

      if (result.GetKind() == TokenKind::kEOF &&
          !include_macro_stack_.empty()) {
        PopIncludeMacroStack();
        continue;
      }

      return;
    }

    result = Token{SourceLocation{}, TokenKind::kEOF, nullptr, 0u};

    return;
  }
}

void Preprocessor::PushIncludeMacroStack() {
  include_macro_stack_.push_back(IncludeStackInfo{
      cur_lexer_callback_, std::move(cur_lexer_), std::move(cur_token_lexer_)});
}

void Preprocessor::PopIncludeMacroStack() {
  cur_lexer_ = std::move(include_macro_stack_.back().lexer);
  cur_token_lexer_ = std::move(include_macro_stack_.back().token_lexer);
  cur_lexer_callback_ = include_macro_stack_.back().lexer_callback;
  include_macro_stack_.pop_back();
}

void Preprocessor::RemoveTopOfLexerStack() {
  assert(!include_macro_stack_.empty() && "lexer stack underflow");

  // When an object-like macro with empty body is exhausted, its leading space
  // should flow through to the next source token (e.g. `EMPTY;` with leading
  // space before EMPTY → the `;` inherits it).
  bool restore_leading = false;
  bool restore_sol = false;

  if (cur_token_lexer_ && !cur_token_lexer_->IsStream()) {
    restore_leading = cur_token_lexer_->HasUnconsumedLeadingSpace();
    restore_sol = cur_token_lexer_->HasUnconsumedStartOfLine();
  }

  // Destroy the exhausted token lexer first: its destructor re-enables the
  // macro it was expanding. Then restore the lexer beneath it.
  cur_token_lexer_.reset();
  PopIncludeMacroStack();

  if (restore_leading) {
    if (cur_token_lexer_) {
      cur_token_lexer_->InheritLeadingSpaceForNext();
    } else if (cur_lexer_) {
      cur_lexer_->SetHasLeadingSpace(true);
    }
  }

  if (restore_sol && cur_token_lexer_) {
    cur_token_lexer_->InheritStartOfLineForNext();
  }
}

Token Preprocessor::LexImpl() {
  assert((cur_lexer_ || cur_token_lexer_) &&
         "Lex() called before entering a source file");

  // Loop until a callback yields a token; a callback returns false when it
  // only reshaped the lexer stack (e.g. popped an exhausted #include).
  Token result;

  while (!cur_lexer_callback_(*this, result));

  return result;
}

Token Preprocessor::Lex() {
  Token result;

  if (cached_lex_pos_ < cached_tokens_.size()) {
    // Replay a token previously produced by LookAhead or retained for a
    // backtrack scope.
    result = cached_tokens_[cached_lex_pos_++];
  } else {
    result = LexImpl();

    // While a backtrack mark is active, every consumed token must be retained.
    if (!backtrack_positions_.empty()) {
      cached_tokens_.push_back(result);
      cached_lex_pos_ = cached_tokens_.size();
    }
  }

  // Once nothing is pending replay and no mark is active, the cache is dead.
  if (backtrack_positions_.empty() &&
      cached_lex_pos_ == cached_tokens_.size()) {
    cached_tokens_.clear();
    cached_lex_pos_ = 0;
  }

  return result;
}

Token Preprocessor::LookAhead(unsigned n) {
  // Materialize enough tokens that index cached_lex_pos_ + n exists, without
  // advancing the consumption cursor.
  while (cached_lex_pos_ + n >= cached_tokens_.size()) {
    cached_tokens_.push_back(LexImpl());
  }

  return cached_tokens_[cached_lex_pos_ + n];
}

void Preprocessor::EnableBacktrackAtThisPos() {
  backtrack_positions_.push_back(cached_lex_pos_);
}

void Preprocessor::CommitBacktrackedTokens() {
  assert(!backtrack_positions_.empty() && "no backtrack mark to commit");

  backtrack_positions_.pop_back();
}

void Preprocessor::Backtrack() {
  assert(!backtrack_positions_.empty() && "no backtrack mark to return to");

  cached_lex_pos_ = backtrack_positions_.back();
  backtrack_positions_.pop_back();
}

bool Preprocessor::CheckPoisonedIdentifier(const IdentifierInfo* ii,
                                           SourceLocation loc) {
  if (ii != nullptr && poisoned_identifiers_.count(ii) > 0) {
    diags_.Report(loc, diag::err_pp_poisoned_macro) << ii->GetName();
    return true;
  }

  return false;
}

bool Preprocessor::CLK_Lexer(Preprocessor& pp, Token& result) {
  Token token = pp.cur_lexer_->Lex();

  if (token.GetKind() == TokenKind::kEOF) {
    return pp.HandleEndOfFile(token, result);
  }

  // A '#' at the start of a line introduces a preprocessing directive.
  if (token.GetKind() == TokenKind::kHash && token.IsStartOfLine()) {
    pp.HandleDirective(token);

    return false;  // Directive consumed; re-dispatch for the next token.
  }

  // This is body content, not a directive. Any such token disqualifies the
  // include-guard optimization unless it sits inside the file's guard.
  pp.cur_lexer_->GetMIOpt().ReadToken();

  if (token.GetKind() == TokenKind::kIdentifier) {
    // Macro expansion started; re-dispatch to drain it.
    if (pp.HandleIdentifier(token)) return false;
  }

  result = token;

  return true;
}

bool Preprocessor::CLK_TokenLexer(Preprocessor& pp, Token& result) {
  Token token{SourceLocation{}, TokenKind::kUnknown, nullptr, 0u};

  if (!pp.cur_token_lexer_->Lex(token)) {
    // Expansion exhausted: drop this token lexer and resume the one beneath.
    pp.RemoveTopOfLexerStack();

    return false;
  }

  // Rescan expansion output for further macros. Identifiers here already carry
  // their IdentifierInfo (attached when the macro body was defined).
  if (token.GetIdentifierInfo() != nullptr && !token.IsDisableExpand()) {
    if (pp.HandleIdentifier(token)) {
      return false;
    }
  }

  result = token;

  return true;
}

bool Preprocessor::HandleEndOfFile(const Token& eof_tok, Token& result) {
  assert(cur_lexer_ && "EOF handling requires an active file lexer");

  // A conditional left open when the file ends is an error.
  if (cur_lexer_->GetConditionalStackDepth() > 0) {
    diags_.Report(cur_lexer_->PeekConditionalLevel().if_loc,
                  diag::err_pp_unterminated_conditional);
  }

  // Record the include-guard controlling macro learned while lexing this file,
  // so a later #include of it can be skipped.
  if (const FileEntry* fe = FileEntryOf(cur_lexer_.get())) {
    if (const IdentifierInfo* m =
            cur_lexer_->GetMIOpt().GetControllingMacro()) {
      header_search_.SetFileControllingMacro(fe, m);
    }
  }

  if (!include_macro_stack_.empty()) {
    // Resume the file that #included this one; re-dispatch so its lexer runs.
    FileID exited_fid = cur_lexer_->GetFileID();
    PopIncludeMacroStack();

    if (callbacks_) {
      callbacks_->FileChanged(sm_.GetLocForStartOfFile(cur_lexer_->GetFileID()),
                              PPCallbacks::FileChangeReason::kExitFile,
                              exited_fid, GetCurrentFileCharacteristic());
    }

    return false;
  }

  // Outermost file exhausted: this is the true end of the translation unit.
  result = eof_tok;

  return true;
}

IdentifierInfo* Preprocessor::LookUpIdentifierInfo(Token& tok) {
  IdentifierInfo* info;

  if (tok.NeedsCleaning()) [[unlikely]] {
    info = &identifiers_.Get(RemoveLineSplices(tok.GetLexeme()));
  } else {
    info = &identifiers_.Get(tok.GetLexeme());
  }

  tok.SetIdentifierInfo(info);
  // Promote to the keyword kind when the spelling is a keyword; a no-op
  // (kIdentifier -> kIdentifier) otherwise.
  tok.SetKind(info->GetTokenKind());

  return info;
}

const FileEntry* Preprocessor::FileEntryOf(
    const PPLexer* lexer) const noexcept {
  assert(lexer != nullptr && "Lexer must not be null");

  // HeaderSearch bookkeeping applies only to included headers. Represent the
  // translation unit's main file as having no associated header entry.
  if (lexer->GetFileID() == sm_.GetMainFileID()) return nullptr;

  return sm_.GetFileEntryForID(lexer->GetFileID());
}

CharacteristicKind Preprocessor::GetCurrentFileCharacteristic() const noexcept {
  assert(cur_lexer_ && "file characteristic requires an active file lexer");

  return header_search_.GetFileCharacteristic(FileEntryOf(cur_lexer_.get()));
}

}  // namespace bcc
