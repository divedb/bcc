#pragma once

#include <cstdint>
#include <vector>

#include "bcc/basic/file_id.hh"
#include "bcc/lex/cursor.hh"
#include "bcc/lex/token.hh"

namespace bcc {

class DiagnosticsEngine;
class SourceManager;

class BufferedLexer {
 public:
  /// \brief Constructs a BufferedLexer for the given file.
  ///
  /// \param sm    The source manager that owns the file being lexed.
  /// \param fid   The FileID of the file to lex.
  /// \param diag  Optional diagnostics engine. When non-null, the lexer emits
  ///              structured diagnostics for malformed input instead of
  ///              silently returning kUnknown tokens.
  explicit BufferedLexer(SourceManager& sm, FileID fid,
                         DiagnosticsEngine* diag = nullptr);

  BufferedLexer(const BufferedLexer&) = delete;
  BufferedLexer& operator=(const BufferedLexer&) = delete;
  BufferedLexer(BufferedLexer&&) = delete;
  BufferedLexer& operator=(BufferedLexer&&) = delete;

  /// \brief Returns the next preprocessing token from the input stream.
  ///
  /// Advances internal lexer state and returns tokens in source order until
  /// TokenKind::kEOF is reached. The returned token preserves its original
  /// source spelling, including any whitespace, comments, or escaped newlines
  /// that belong to that token.
  Token NextToken();

  /// \brief Sets the leading-space flag that will be applied to the next token.
  void SetHasLeadingSpace(bool v) noexcept { has_leading_space_ = v; }

 private:
  friend class PPLexer;

  /// Returns the next token, recognizing an opening '<' or '"' as the start of
  /// a header name. This contextual entry point is restricted to PPLexer.
  Token LexHeaderName();
  Token Lex(bool recognize_header_name);

  void InitializeTokenFlags() noexcept;
  void UpdateLexerState(TokenKind kind) noexcept;

  Token LexToken(bool recognize_header_name) noexcept;
  Token LexHeaderNameBody(Cursor cursor, char close) noexcept;
  Token LexNumericConstant(Cursor cursor) noexcept;
  Token LexPPNumberOrPeriod(Cursor cursor, uint32_t lead) noexcept;
  Token LexPunctuator(Cursor cursor, uint32_t lead) noexcept;
  Token LexIdentifier(Cursor cursor) noexcept;
  Token LexDelimitedLiteral(Cursor cursor, TokenKind kind,
                            char delimiter) noexcept;
  Token LexDelimitedLiteralOrIdentifier(Cursor cursor, uint32_t lead) noexcept;
  Token LexMultiLineComment(Cursor cursor) noexcept;
  Token LexSingleLineComment(Cursor cursor) noexcept;
  Token LexCommentOrSlash(Cursor cursor) noexcept;
  Token LexNewLine(Cursor cursor, uint32_t lead) noexcept;
  Token LexWhiteSpace(Cursor cursor) noexcept;
  Token EOFToken() noexcept;
  Token FinalizeToken(TokenKind kind, Cursor cursor) noexcept;

  //===------------------------------------------------------------------===//
  // Version-control conflict marker recovery.
  //===------------------------------------------------------------------===//

  // A byte range to skip over (the non-"ours" sections of a conflict block).
  struct ConflictSkip {
    const char* start;
    const char* end;
  };

  // If the cursor sits at the start of a pending conflict-skip range, advance
  // past it. Returns true if the cursor moved.
  bool ApplyConflictSkips() noexcept;

  // Detects a `<<<<<<<` / `>>>> ` conflict marker at the start of the current
  // line, records the skip ranges for the non-"ours" sections, and advances
  // the cursor past the start marker line. Returns true if handled.
  bool TryConflictMarker() noexcept;

  std::vector<ConflictSkip> conflict_skips_;

  /// \brief Returns the source location of the first byte of the token
  ///        currently being lexed (i.e. cursor_'s position before any
  ///        look-ahead has been committed via FinalizeToken).
  SourceLocation CurrentTokenLoc() const noexcept;

  SourceManager& sm_;
  FileID fid_;
  DiagnosticsEngine* diag_;
  Cursor cursor_;
  TokenFlag current_token_flags_;
  bool is_at_start_of_line_;
  bool has_leading_space_;
};

}  // namespace bcc
