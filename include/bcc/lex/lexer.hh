#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "bcc/lex/cursor.hh"
#include "bcc/lex/source_buffer.hh"
#include "bcc/lex/token.hh"

namespace bcc {

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
  explicit BufferedLexer(const SourceBuffer& buffer)
      : cursor_(buffer.Data()),
        current_token_flags_(TokenFlag::kNone),
        is_at_start_of_line_(true),
        has_leading_space_(false) {}

  Token NextToken() override;

 private:
  void InitializeTokenFlags() noexcept;
  void UpdateLexerState(TokenKind kind) noexcept;
  void SkipIgnoredNullBytes() noexcept;

  Token LexToken();
  Token LexNumericConstant(Cursor cursor);
  Token LexPPNumberOrPeriod(Cursor cursor, uint32_t lead);
  Token LexPunctuator(Cursor cursor, uint32_t lead);
  Token LexIdentifier(Cursor cursor);
  Token LexDelimitedLiteral(Cursor cursor, TokenKind kind, char delimiter);
  Token LexDelimitedLiteralOrIdentifier(Cursor cursor, uint32_t lead);
  Token LexMultiLineComment(Cursor cursor);
  Token LexSingleLineComment(Cursor cursor);
  Token LexCommentOrSlash(Cursor cursor);
  Token LexNewLine(Cursor cursor, uint32_t lead);
  Token LexWhiteSpace(Cursor cursor);
  Token EOFToken();
  Token FinalizeToken(TokenKind kind, Cursor cursor);

  Cursor cursor_;
  TokenFlag current_token_flags_;
  bool is_at_start_of_line_;
  bool has_leading_space_;
};

}  // namespace bcc
