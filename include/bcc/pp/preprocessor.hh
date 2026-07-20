#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "bcc/basic/characteristic_kind.hh"
#include "bcc/basic/file_id.hh"
#include "bcc/basic/source_location.hh"
#include "bcc/lex/token.hh"
#include "bcc/pp/identifier_table.hh"
#include "bcc/pp/pp_lexer.hh"
#include "bcc/pp/scratch_buffer.hh"

namespace bcc {

class DiagnosticsEngine;
class FileEntry;
class HeaderSearch;
class MacroArgs;
class MacroInfo;
class MacroDirective;
class PPCallbacks;
class SourceManager;
class TokenLexer;

/// \brief Drives preprocessing: streams tokens from a stack of source files.
///
/// The Preprocessor is the object a client pulls tokens from, one at a time,
/// via Lex(). It owns the stack of lexers currently in play — the file being
/// lexed plus any files it #included — and the IdentifierTable that interns
/// every identifier spelling.
///
/// Token production is dispatched through a lexer-kind callback so that the
/// main Lex() loop never has to branch on the stack shape and never recurses
/// across #include or (later) macro-expansion boundaries. This mirrors Clang's
/// Preprocessor. At this stage only file lexing (CLK_Lexer) exists; macro
/// token replay (CLK_TokenLexer) and directive handling arrive in later phases.
class Preprocessor {
 public:
  Preprocessor(SourceManager& sm, DiagnosticsEngine& diags,
               HeaderSearch& header_search);

  Preprocessor(const Preprocessor&) = delete;
  Preprocessor& operator=(const Preprocessor&) = delete;

  ~Preprocessor();

  SourceManager& GetSourceManager() const noexcept { return sm_; }
  DiagnosticsEngine& GetDiagnostics() const noexcept { return diags_; }
  IdentifierTable& GetIdentifierTable() noexcept { return identifiers_; }

  /// \brief Installs the observer that receives preprocessor events.
  ///
  /// The Preprocessor takes ownership. Passing nullptr detaches any observer.
  void SetPPCallbacks(std::unique_ptr<PPCallbacks> callbacks) noexcept;
  PPCallbacks* GetPPCallbacks() const noexcept { return callbacks_.get(); }

  /// \brief Enters the SourceManager's main file to begin preprocessing.
  ///
  /// \pre The main FileID has been set on the SourceManager.
  void EnterMainFile();

  /// \brief Pushes \p fid onto the lexer stack and begins lexing it.
  ///
  /// Any file currently being lexed is suspended and resumes once \p fid
  /// reaches end of file.
  ///
  /// \param include_loc Location of the #include that pulled in this file, or
  ///                    an invalid location for the main file.
  void EnterSourceFile(FileID fid, SourceLocation include_loc = {});

  /// \brief Returns the next token of the translation unit.
  ///
  /// Trivia has already been stripped by the underlying PPLexer. Identifier
  /// tokens are annotated with their IdentifierInfo and promoted to their
  /// keyword kind where applicable. At true end of input a kEOF token is
  /// returned repeatedly.
  Token Lex();

  //===--------------------------------------------------------------------===//
  // Lookahead and backtracking (for the parser).
  //===--------------------------------------------------------------------===//

  /// \brief Returns the token \p n positions ahead of the next Lex() without
  ///        consuming it. LookAhead(0) peeks the next token.
  ///
  /// Peeked tokens are cached and replayed by subsequent Lex() calls, so this
  /// never loses input.
  Token LookAhead(unsigned n);

  /// \brief Marks the current position so lexing can later return to it.
  ///
  /// Every token read after this call is cached. A matching Backtrack() rewinds
  /// to here; a matching CommitBacktrackedTokens() drops the mark and keeps the
  /// tokens consumed. Calls may nest.
  void EnableBacktrackAtThisPos();

  /// \brief Discards the innermost backtrack mark, keeping consumed tokens.
  void CommitBacktrackedTokens();

  /// \brief Rewinds to the innermost backtrack mark.
  void Backtrack();

  /// \brief Whether a backtrack mark is currently active.
  bool IsBacktrackEnabled() const noexcept {
    return !backtrack_positions_.empty();
  }

  /// \brief Interns \p tok's spelling, annotating it with its IdentifierInfo
  ///        and promoting its kind to the corresponding keyword kind.
  ///
  /// \return The IdentifierInfo for the token's spelling.
  IdentifierInfo* ResolveIdentifier(Token& tok);

  //===--------------------------------------------------------------------===//
  // Macro state.
  //===--------------------------------------------------------------------===//

  /// \brief The current definition of the macro named \p ii, or nullptr if
  ///        \p ii is not currently defined as a macro.
  MacroInfo* GetMacroInfo(const IdentifierInfo* ii) const;

  /// \brief Whether \p ii currently names a defined macro.
  bool IsMacroDefined(const IdentifierInfo* ii) const {
    return GetMacroInfo(ii) != nullptr;
  }

  /// \brief Calls \p visitor once for each macro that is currently defined.
  ///
  /// Used by `-dM` (and `-dD`) to enumerate the final macro table. The visitor
  /// receives the macro's name and its active MacroInfo. Order is unspecified.
  void ForEachDefinedMacro(
      std::function<void(const IdentifierInfo*, const MacroInfo*)> visitor)
      const;

  ScratchBuffer& GetScratchBuffer() noexcept { return scratch_; }

  /// \brief Checks if \p ii has been poisoned; if so, emits an error.
  ///
  /// \return True if the identifier is poisoned (caller should skip it).
  bool CheckPoisonedIdentifier(const IdentifierInfo* ii, SourceLocation loc);

  /// \brief Fully macro-expands a function-like macro argument.
  ///
  /// Used by MacroArgs to lazily pre-expand an argument before it is
  /// substituted into a replacement list.
  std::vector<Token> ExpandArgument(const std::vector<Token>& arg_tokens);

  /// \brief The FileID of the file currently being lexed, or an invalid FileID
  ///        if no file has been entered.
  FileID GetCurrentFileID() const noexcept {
    return cur_lexer_ ? cur_lexer_->GetFileID() : FileID{};
  }

  /// \brief The system-header characteristic of the file currently being lexed.
  ///
  /// Derived from HeaderSearch's per-file record: a file found on the angled
  /// (system) search path, or promoted by `#pragma GCC system_header`, reports
  /// kSystem. The main file reports kUser.
  ///
  /// \pre A source file lexer is currently active.
  CharacteristicKind GetCurrentFileCharacteristic() const noexcept;

  /// \brief Whether the file currently being lexed is a system header.
  bool IsInSystemHeader() const noexcept {
    return IsSystemHeader(GetCurrentFileCharacteristic());
  }

  /// \brief Depth of the #include stack, not counting the current file.
  unsigned GetIncludeStackDepth() const noexcept {
    return static_cast<unsigned>(include_macro_stack_.size());
  }

 private:
  /// A lexer-kind callback fills \p result and returns true when it produced a
  /// token, or returns false when it changed the lexer stack (e.g. popped an
  /// exhausted file) and the driver should re-dispatch.
  using LexerCallback = bool (*)(Preprocessor&, Token&);

  /// Pulls the next token from the current file lexer.
  static bool CLK_Lexer(Preprocessor& pp, Token& result);

  /// Pulls the next token from the current macro-expansion token lexer.
  static bool CLK_TokenLexer(Preprocessor& pp, Token& result);

  /// Runs the lexer-callback dispatch loop to produce the next token directly
  /// from the lexer stack, without the caching/replay layer that Lex() adds.
  Token LexImpl();

  /// Handles a file lexer reaching end of file: pops back to the includer and
  /// returns false to re-dispatch, or returns true (with \p result set to the
  /// EOF token) when the outermost file is exhausted.
  bool HandleEndOfFile(const Token& eof_tok, Token& result);

  /// Lazily materializes the translation-unit-wide __DATE__ and __TIME__
  /// spellings in scratch storage.
  void EnsureDateTimeTokens();

  //===--------------------------------------------------------------------===//
  // Directive handling (pp_directives.cc).
  //===--------------------------------------------------------------------===//

  /// Handles a '#' seen at the start of a line. Reads the directive name and
  /// dispatches; consumes through the end-of-directive token.
  void HandleDirective(Token& hash_tok);
  /// Dispatches a directive whose first token is an identifier.
  void HandleNamedDirective(Token& directive);
  void NoteNonGuardDirectiveAtFileScope();
  /// Reads and validates the unexpanded macro name following #define/#undef.
  /// On failure, diagnoses the token and consumes the rest of the directive.
  IdentifierInfo* ReadMacroName(Token& name);
  void HandleDefineDirective(Token& define_tok);
  void HandleUndefDirective(Token& undef_tok);

  //===--------------------------------------------------------------------===//
  // Source file inclusion (pp_directives.cc).
  //===--------------------------------------------------------------------===//

  /// Handles #include: resolves the header name, applies the multiple-include
  /// optimization, and enters the file (or reports it as not found).
  void HandleIncludeDirective(Token& include_tok);

  /// Handles #include_next: like HandleIncludeDirective but searches
  /// directories after the includer's directory in the search path.
  void HandleIncludeNextDirective(Token& include_next_tok);

  /// Handles #import: like HandleIncludeDirective but marks the file as
  /// imported so subsequent #import of it is a no-op.
  void HandleImportDirective(Token& import_tok);

  /// Handles __include_macros: include a file but only process its macro
  /// definitions.
  void HandleIncludeMacrosDirective(Token& include_macros_tok);

  /// Interprets a macro-expanded token sequence as a #include header name
  /// (a single string literal or a `<...>` group).
  bool InterpretExpandedHeader(const std::vector<Token>& toks,
                               std::string& filename, bool& is_angled);

  enum class IncludeKind { kInclude, kIncludeNext, kImport, kIncludeMacros };

  struct HeaderName {
    std::string filename;
    SourceLocation location;
    bool is_angled = false;
  };

  enum class HeaderEntryDecision {
    kEnter,
    kSkipAlreadyImported,
    kSkipPragmaOnce,
    kSkipControllingMacro,
    kIncludeDepthExceeded,
  };

  std::optional<HeaderName> ParseHeaderName();
  const FileEntry* LookupHeader(const HeaderName& header, IncludeKind kind);
  HeaderEntryDecision DecideHeaderEntry(const FileEntry* file,
                                        IncludeKind kind) const;
  bool EnterResolvedHeader(const FileEntry* file, SourceLocation loc,
                           IncludeKind kind);

  /// Common include logic shared by #include, #include_next, #import, and
  /// __include_macros. Returns true if the file was entered.
  bool HandleIncludeCommon(Token& include_tok, IncludeKind kind);

  /// Handles #pragma. Recognizes `#pragma once`, `#pragma GCC poison`,
  /// `#pragma GCC system_header`, `#pragma GCC dependency`,
  /// `#pragma GCC diagnostic`, `#pragma push_macro`, `#pragma pop_macro`,
  /// and `#pragma STDC`.
  void HandlePragmaDirective(Token& pragma_tok);

  /// Handles #ident / #sccs (both are synonyms). Discards the string argument.
  void HandleIdentDirective(Token& ident_tok);

  /// Sub-dispatch for `#pragma GCC` / `#pragma clang`.
  void HandleGCCOrClangPragma(Token& pragma_tok, Token& ns_tok);

  /// Handles #pragma GCC poison / #pragma clang poison.
  void HandlePragmaPoison(Token& pragma_tok);

  /// Handles #pragma GCC dependency.
  void HandlePragmaDependency();

  /// Handles #pragma GCC diagnostic / #pragma clang diagnostic.
  void HandlePragmaDiagnostic();

  /// Handles #pragma STDC FENV_ACCESS / FP_CONTRACT / CX_LIMITED_RANGE.
  void HandleSTDCPragma(Token& pragma_tok);

  /// Handles #pragma message, #pragma GCC warning, and #pragma GCC error by
  /// concatenating their (macro-expanded) string arguments and re-emitting the
  /// pragma with a single re-encoded string, matching Clang's -E output.
  void HandlePragmaMessageLike(const Token& pragma_tok, std::string_view kind,
                               bool is_gcc);

  /// Implements the _Pragma("...") operator: decodes the string and processes
  /// it as a #pragma directive. Returns true if it was a _Pragma(...) call
  /// (and thus the _Pragma token should not be emitted).
  bool HandlePragmaOperator(Token& tok);

  /// Handles #pragma push_macro("name").
  void HandlePushMacroPragma();

  /// Handles #pragma pop_macro("name").
  void HandlePopMacroPragma();

  /// Directory containing the file currently being lexed, for resolving quoted
  /// includes. Empty when unknown (e.g. an in-memory buffer with no path).
  std::string_view GetCurrentFileDir() const;

  //===--------------------------------------------------------------------===//
  // Line control and user diagnostics (pp_directives.cc).
  //===--------------------------------------------------------------------===//

  /// Handles #line: parses a (macro-expanded) line number and optional filename
  /// and records the override in the SourceManager's line table.
  void HandleLineDirective(Token& line_tok);
  void HandleLineMarkerDirective(Token& line_number);
  void HandleLineControlDirective(Token& directive, Token line_number,
                                  bool expand_tokens);

  /// Handles #error (is_error) and #warning by emitting the rest of the line as
  /// a user diagnostic through the DiagnosticsEngine.
  void HandleUserDiagnosticDirective(Token& tok, bool is_error);
  /// Reads and validates a function-like macro's parameter list.
  /// Returns false after diagnosing and recovering from malformed input.
  bool ReadMacroParameterList(MacroInfo* macro);
  /// Consumes the remainder of the directive unless \p current already ended
  /// it. Safe to call uniformly from token-validation error paths.
  void DiscardDirectiveRemainder(const Token& current);
  /// Consumes tokens from the current directive line up to and including kEod.
  void DiscardUntilEndOfDirective();

  //===--------------------------------------------------------------------===//
  // Conditional inclusion (#if family — pp_directives.cc / pp_expressions.cc).
  //===--------------------------------------------------------------------===//

  void HandleIfDirective(Token& if_tok);
  void HandleIfdefDirective(Token& tok, bool is_ifndef);
  void HandleElifFamilyDirective(Token& tok, PPKeyword kw);
  void HandleElseDirective(Token& else_tok);
  void HandleEndifDirective(Token& endif_tok);

  /// Skips tokens from the current point until the #else/#elif that becomes
  /// active or the matching #endif of the innermost open conditional, tracking
  /// nested conditionals and without expanding macros or acting on directives.
  void SkipExcludedConditionalBlock();

  /// Reads the macro-name operand of #ifdef/#ifndef/#elifdef/#elifndef
  /// (unexpanded) and reports whether it is currently defined.
  bool CheckIsMacroDefinedOperand(IdentifierInfo** out_name = nullptr);

  /// Evaluates the controlling constant expression of a #if / #elif, consuming
  /// the rest of the directive line.
  bool EvaluateDirectiveExpression();

  /// Lexes the next macro-expanded token of the current directive line.
  Token LexDirectiveToken();

  /// Like LexDirectiveToken but resolves a `defined` operator into a 0/1
  /// numeric token (its operand is not macro-expanded).
  Token LexConditionToken();

  /// Reads and evaluates a `defined X` / `defined(X)` operator.
  bool EvaluateDefinedOperator();

  /// Reads and evaluates a `__has_builtin(X)` or `__has_attribute(X)`
  /// expression, consuming the paren-delimited argument and returning a "0"
  /// or "1" numeric token.
  Token EvaluateHasExpression(Token& tok);

  /// Reads and evaluates `__is_identifier(X)`: returns "1" if X is a plain
  /// identifier in the current language (not a keyword), else "0".
  Token EvaluateIsIdentifier(Token& tok);

  //===--------------------------------------------------------------------===//
  // Macro expansion (pp_macro_expansion.cc).
  //===--------------------------------------------------------------------===//

  /// Ensures \p tok carries its IdentifierInfo and, if it names an expandable
  /// macro, begins expanding it.
  ///
  /// \return True if a macro expansion was started (the caller should
  ///         re-dispatch); false if \p tok should be returned as-is.
  bool HandleIdentifier(Token& tok);

  /// Attempts to expand \p tok as a macro. Paints \p tok with kDisableExpand
  /// when it names a macro that is currently being expanded.
  bool TryExpandMacro(Token& tok);

  /// Pushes a TokenLexer for \p macro (invoked at \p name_tok) onto the stack.
  /// \param args Actual arguments for a function-like macro, else nullptr.
  void EnterMacro(Token& name_tok, MacroInfo* macro, MacroArgs* args);

  /// Registers the builtin macros (__LINE__, __FILE__, ...) at construction.
  void RegisterBuiltinMacros();

  /// Registers one builtin macro, returning its IdentifierInfo for O(1)
  /// dispatch in ExpandBuiltinMacro.
  IdentifierInfo* RegisterBuiltin(const char* name);

  /// Rewrites \p tok in place with the value of the builtin macro it names
  /// (e.g. __LINE__ becomes a numeric-constant token). Called from the
  /// expansion path for identifiers whose MacroInfo is a builtin.
  void ExpandBuiltinMacro(Token& tok);

  /// Peeks the next token: if it is '(', consumes it and returns true;
  /// otherwise pushes it back (unexpanded) and returns false. Used to decide
  /// whether a function-like macro name is actually a macro call.
  bool IsNextTokenLParen();

  /// Collects the arguments of a function-like macro invocation. The opening
  /// '(' must already have been consumed; consumes through the closing ')'.
  MacroArgs ReadFunctionLikeMacroArgs(MacroInfo* macro);

  /// Reads the next token directly from the lexer stack, crossing
  /// file/expansion boundaries, without macro expansion or directive handling.
  /// Used for argument collection and lookahead.
  void LexUnexpandedToken(Token& result);

  MacroInfo* AllocateMacroInfo(SourceLocation loc);
  void AppendDefMacroDirective(IdentifierInfo* ii, MacroInfo* macro,
                               SourceLocation loc);
  void AppendUndefMacroDirective(IdentifierInfo* ii, SourceLocation loc);

  //===--------------------------------------------------------------------===//
  // Lexer stack management.
  //===--------------------------------------------------------------------===//

  /// Pushes \p fid onto the lexer stack in response to an #include.
  void EnterIncludeFile(FileID fid, SourceLocation include_loc);

  void EnterSourceFileWithLexer(std::unique_ptr<PPLexer> lexer);
  void EnterTokenStream(std::unique_ptr<TokenLexer> token_lexer);
  /// Pushes a raw token stream (owning \p tokens) onto the lexer stack.
  void EnterTokenStream(std::vector<Token> tokens);
  void PushIncludeMacroStack();
  void PopIncludeMacroStack();
  /// Discards the exhausted top token lexer and restores the lexer beneath it.
  void RemoveTopOfLexerStack();

  /// Returns the file backing \p lexer. The main file is intentionally
  /// excluded because header-search state applies only to included files.
  const FileEntry* FileEntryOf(const PPLexer* lexer) const noexcept;

  /// One suspended lexer on the stack. Exactly one of lexer / token_lexer is
  /// non-null, matching which callback was active.
  struct IncludeStackInfo {
    LexerCallback lexer_callback;
    std::unique_ptr<PPLexer> lexer;
    std::unique_ptr<TokenLexer> token_lexer;
  };

  /// Maximum #include nesting depth, mirroring Clang's limit. Guards against
  /// runaway recursive inclusion.
  static constexpr unsigned kMaxIncludeDepth = 200;

  SourceManager& sm_;
  DiagnosticsEngine& diags_;
  HeaderSearch& header_search_;
  std::unique_ptr<PPCallbacks> callbacks_;
  IdentifierTable identifiers_;
  ScratchBuffer scratch_;

  //===--------------------------------------------------------------------===//
  // Token cache backing LookAhead / backtracking.
  //===--------------------------------------------------------------------===//

  /// Tokens lexed ahead of consumption (via LookAhead) or retained for
  /// backtracking. Replayed by Lex() before pulling new tokens.
  std::vector<Token> cached_tokens_;

  /// Index of the next cached token Lex() will replay.
  std::size_t cached_lex_pos_ = 0;

  /// Positions in cached_tokens_ marked by EnableBacktrackAtThisPos(), one per
  /// active (possibly nested) backtrack scope.
  std::vector<std::size_t> backtrack_positions_;

  /// The file currently being lexed, or null while expanding a macro.
  std::unique_ptr<PPLexer> cur_lexer_;

  /// The macro currently being expanded, or null while lexing a file.
  std::unique_ptr<TokenLexer> cur_token_lexer_;
  LexerCallback cur_lexer_callback_ = &CLK_Lexer;

  /// Suspended lexers, innermost last. Does not include the current lexer.
  std::vector<IncludeStackInfo> include_macro_stack_;

  /// Monotonic source of __COUNTER__ values.
  unsigned counter_ = 0;

  /// Scratch spelling locations for __DATE__ / __TIME__. Initialized together
  /// on first use so every expansion in the translation unit agrees.
  SourceLocation date_loc_;
  SourceLocation time_loc_;

  /// IdentifierInfo pointers for builtin macros — enables O(1) dispatch in
  /// ExpandBuiltinMacro (pointer comparison) instead of string comparison.
  IdentifierInfo* ident_line_ = nullptr;
  IdentifierInfo* ident_file_ = nullptr;
  IdentifierInfo* ident_date_ = nullptr;
  IdentifierInfo* ident_time_ = nullptr;
  IdentifierInfo* ident_counter_ = nullptr;
  IdentifierInfo* ident_include_level_ = nullptr;
  IdentifierInfo* ident_stdc_ = nullptr;
  IdentifierInfo* ident_stdc_hosted_ = nullptr;
  IdentifierInfo* ident_stdc_version_ = nullptr;
  IdentifierInfo* ident_base_file_ = nullptr;
  IdentifierInfo* ident_file_name_ = nullptr;
  IdentifierInfo* ident_timestamp_ = nullptr;
  IdentifierInfo* ident_flt_eval_method_ = nullptr;
  IdentifierInfo* ident_pragma_ = nullptr;
  IdentifierInfo* ident_bitint_maxwidth_ = nullptr;
  IdentifierInfo* ident_char16_type_ = nullptr;
  IdentifierInfo* ident_char32_type_ = nullptr;
  IdentifierInfo* ident_wchar_max_ = nullptr;

  /// Identifiers poisoned by `#pragma GCC poison`. A poisoned identifier may
  /// never be macro-expanded, used in `#ifdef`, `#if defined`, or appear as a
  /// token.
  std::set<const IdentifierInfo*> poisoned_identifiers_;

  /// Macro stack for `#pragma push_macro` / `#pragma pop_macro`.
  /// Maps identifier name -> stack of MacroDirective* snapshots.
  std::unordered_map<std::string, std::vector<MacroDirective*>>
      macro_pragma_stack_;

  /// Latest #define/#undef directive per identifier (head of its history).
  std::unordered_map<const IdentifierInfo*, MacroDirective*> macros_;

  /// Backing storage owning every MacroInfo / MacroDirective.
  std::vector<std::unique_ptr<MacroInfo>> macro_infos_;
  std::vector<std::unique_ptr<MacroDirective>> macro_directives_;
};

}  // namespace bcc
