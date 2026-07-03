#pragma once

#include <cassert>
#include <cstdint>
#include <vector>

#include "bcc/basic/file_id.hh"
#include "bcc/basic/source_location.hh"
#include "bcc/lex/lexer.hh"
#include "bcc/lex/token.hh"

namespace bcc {

class DiagnosticsEngine;
class FileEntry;
class IdentifierInfo;
class SourceManager;

/// \brief Detects the include-guard "controlling macro" of a source file.
///
/// A file whose entire body is wrapped in
///
///     #ifndef GUARD
///     #define GUARD
///     ... file body ...
///     #endif
///
/// produces no tokens on any inclusion after the first (GUARD becomes defined
/// during the first inclusion). Recognizing this "controlling macro" lets the
/// preprocessor skip re-reading such a file entirely.
///
/// The detector is deliberately conservative: any deviation from the exact
/// shape above marks the file invalid, so a controlling macro is reported only
/// when skipping the file on re-inclusion is guaranteed safe. The preprocessor
/// drives it with the hooks below as it processes the file's directives and
/// content tokens.
class MultipleIncludeOpt {
 public:
  /// Records that a content (non-directive) token was produced from the file.
  void ReadToken() {
    // Content is only expected inside the guard; anywhere else breaks it.
    if (state_ != State::kInGuard) Invalidate();
  }

  /// Records a top-level `#ifndef NAME` (conditional nesting was 0 before it).
  void EnterTopLevelIfndef(const IdentifierInfo* name) {
    if (state_ == State::kStart) {
      guard_macro_ = name;
      state_ = State::kAfterIfndef;
    } else {
      Invalidate();
    }
  }

  /// Records any other top-level conditional (`#if`, `#ifdef`, or a second
  /// `#ifndef`): the file is not guarded by a single macro.
  void EnterOtherTopLevelConditional() { Invalidate(); }

  /// Records a `#define NAME` that is not at file scope (nesting > 0).
  void DefinedMacro(const IdentifierInfo* name) {
    if (state_ == State::kAfterIfndef) {
      // The guard's `#define GUARD` must immediately follow its `#ifndef`.
      state_ = (name == guard_macro_) ? State::kInGuard : State::kInvalid;
    }
    // A #define deeper inside the guarded body is fine and changes nothing.
  }

  /// Records the `#endif` that closed the outermost (file-scope) conditional.
  void ExitTopLevelConditional() {
    state_ = (state_ == State::kInGuard) ? State::kAfterEndif : State::kInvalid;
  }

  /// Records any other file-scope directive that is not part of the guard.
  void OtherTopLevelDirective() {
    if (state_ != State::kInGuard) Invalidate();
  }

  /// Abandons the optimization for this file.
  void Invalidate() { state_ = State::kInvalid; }

  /// The controlling macro if the guard shape held for the whole file, else
  /// nullptr. Call at end of file.
  const IdentifierInfo* GetControllingMacro() const {
    return state_ == State::kAfterEndif ? guard_macro_ : nullptr;
  }

 private:
  enum class State {
    kStart,        ///< Nothing significant seen yet.
    kAfterIfndef,  ///< Saw the leading `#ifndef GUARD`; awaiting `#define`.
    kInGuard,      ///< Saw `#define GUARD`; inside the guarded region.
    kAfterEndif,   ///< The guard's `#endif` closed; awaiting end of file.
    kInvalid,      ///< The guard shape was broken; no optimization.
  };

  State state_ = State::kStart;
  const IdentifierInfo* guard_macro_ = nullptr;
};

/// \brief State for one open #if / #ifdef / #ifndef block within a file.
///
/// Populated later by the directive handler; PPLexer only owns the stack so
/// that conditional nesting is tracked per source file (each #include gets its
/// own PPLexer and therefore its own stack).
struct PPConditionalInfo {
  /// Location of the #if / #ifdef / #ifndef that opened this level.
  SourceLocation if_loc;
  /// Whether tokens were already being skipped when this level was entered.
  bool was_skipping = false;
  /// Whether a non-skipped branch has been taken at this level yet.
  bool found_non_skip = false;
  /// Whether the #else clause of this level has been seen.
  bool found_else = false;
};

/// \brief Preprocessor-facing lexer for a single source file.
///
/// PPLexer wraps a BufferedLexer and adapts its raw preprocessing-token stream
/// (which includes whitespace, comments, and newlines) into the trivia-free
/// stream the preprocessor expects:
///
///   * Whitespace and comment tokens are dropped. Their presence is already
///     reflected in the following token's kLeadingSpace / kStartOfLine flags,
///     which the underlying BufferedLexer maintains, so no flag recomputation
///     is needed here.
///   * Newlines are dropped as well, except while a preprocessing directive is
///     being parsed, in which case the terminating newline becomes a single
///     kEod ("end of directive") token so the preprocessor can find the end of
///     the directive line.
///
/// It also owns the per-file conditional-inclusion stack and knows how to lex
/// an #include header-name (<...> or "...").
class PPLexer {
 public:
  /// \param fe The FileEntry this file was resolved from, or nullptr for the
  ///           main file / in-memory buffers. Used by the multiple-include
  ///           optimization to key per-header state.
  PPLexer(SourceManager& sm, FileID fid, DiagnosticsEngine* diag = nullptr,
          const FileEntry* fe = nullptr);

  PPLexer(const PPLexer&) = delete;
  PPLexer& operator=(const PPLexer&) = delete;

  /// \brief Returns the next non-trivia token.
  ///
  /// While a directive is being parsed (see SetParsingPreprocessorDirective),
  /// the newline (or end of file) that ends the directive is reported as a
  /// kEod token and the directive-parsing flag is cleared.
  Token Lex();

  /// \brief Lexes a header-name token for a #include filename.
  ///
  /// Returns a kHeaderName token for a <...> or "..." filename. For a computed
  /// include (e.g. a macro), returns the ordinary next token instead, leaving
  /// macro expansion to the caller. If the directive line ends immediately, a
  /// kEod token is returned, exactly as Lex() would produce.
  Token LexIncludeFilename();

  FileID GetFileID() const noexcept { return fid_; }

  /// \brief Sets the leading-space flag applied to the next non-trivia token.
  void SetHasLeadingSpace(bool v) noexcept { lexer_.SetHasLeadingSpace(v); }

  /// \brief The FileEntry this file was resolved from, or nullptr.
  const FileEntry* GetFileEntry() const noexcept { return file_entry_; }

  /// \brief The include-guard controlling-macro detector for this file.
  MultipleIncludeOpt& GetMIOpt() noexcept { return mio_; }

  //===--------------------------------------------------------------------===//
  // Directive parsing state.
  //===--------------------------------------------------------------------===//

  /// \brief Controls whether the terminating newline is reported as kEod.
  ///
  /// The preprocessor sets this to true when it begins parsing a directive
  /// (just after the leading '#'). The flag is cleared automatically once the
  /// kEod token is produced.
  void SetParsingPreprocessorDirective(bool value) noexcept {
    parsing_preprocessor_directive_ = value;
  }
  bool IsParsingPreprocessorDirective() const noexcept {
    return parsing_preprocessor_directive_;
  }

  //===--------------------------------------------------------------------===//
  // Conditional (#if / #ifdef / ...) stack.
  //===--------------------------------------------------------------------===//

  void PushConditionalLevel(SourceLocation if_loc, bool was_skipping,
                            bool found_non_skip, bool found_else) {
    conditional_stack_.push_back(
        PPConditionalInfo{if_loc, was_skipping, found_non_skip, found_else});
  }
  void PushConditionalLevel(const PPConditionalInfo& info) {
    conditional_stack_.push_back(info);
  }

  /// \brief Pops the innermost conditional level into \p info.
  ///
  /// \return True if the stack was empty (nothing popped, \p info untouched).
  bool PopConditionalLevel(PPConditionalInfo& info) {
    if (conditional_stack_.empty()) return true;
    info = conditional_stack_.back();
    conditional_stack_.pop_back();
    return false;
  }

  /// \pre The conditional stack is non-empty.
  PPConditionalInfo& PeekConditionalLevel() {
    assert(!conditional_stack_.empty() && "no conditional active");
    return conditional_stack_.back();
  }

  unsigned GetConditionalStackDepth() const noexcept {
    return static_cast<unsigned>(conditional_stack_.size());
  }

 private:
  /// Builds a zero-length kEod marker positioned at \p trivia (the newline or
  /// EOF token that terminated the directive).
  static Token MakeEodToken(const Token& trivia) noexcept;

  BufferedLexer lexer_;
  FileID fid_;
  const FileEntry* file_entry_ = nullptr;
  MultipleIncludeOpt mio_;
  bool parsing_preprocessor_directive_ = false;
  std::vector<PPConditionalInfo> conditional_stack_;
};

}  // namespace bcc
