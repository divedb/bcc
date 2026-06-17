#pragma once

#include <cstdint>

#include "bcc/basic/file_id.hh"
#include "bcc/lex/cursor.hh"
#include "bcc/lex/token.hh"

namespace bcc {

class SourceManager;

class LexerBase {
 public:
  LexerBase() = default;
  LexerBase(const LexerBase&) = delete;
  LexerBase& operator=(const LexerBase&) = delete;
  LexerBase(LexerBase&&) = delete;
  LexerBase& operator=(LexerBase&&) = delete;

  virtual ~LexerBase() = default;

  /// \brief Returns the next preprocessing token from the input stream.
  ///
  /// Implementations advance internal lexer state and return tokens in source
  /// order until TokenKind::kEOF is reached. The returned token preserves its
  /// original source spelling, including any whitespace, comments, or escaped
  /// newlines that belong to that token.
  ///
  /// \return The next token in the input stream, or a token of kind kEOF when
  ///         the end of the input is reached.
  virtual Token NextToken() = 0;
};

class BufferedLexer : public LexerBase {
 public:
  explicit BufferedLexer(SourceManager& sm, FileID fid);

  Token NextToken() override;

 private:
  void InitializeTokenFlags() noexcept;
  void UpdateLexerState(TokenKind kind) noexcept;

  Token LexToken() noexcept;
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

  SourceManager& sm_;
  FileID fid_;
  Cursor cursor_;
  TokenFlag current_token_flags_;
  bool is_at_start_of_line_;
  bool has_leading_space_;
};

}  // namespace bcc
