#pragma once

#include <string_view>
#include <vector>

#include "bcc/as/token.hh"

namespace bcc::as {

/// \brief A hand-written, line-oriented scanner for AT&T x86-64 assembly.
///
/// The lexer is driven token-at-a-time via \ref Next, with one token of
/// lookahead through \ref Peek. It computes numeric values eagerly and decodes
/// string/character escapes, so the parser never re-scans literal text.
class Lexer {
 public:
  explicit Lexer(std::string_view src) : src_(src) {}

  /// Consumes and returns the next token.
  Token Next();

  /// Returns the next token without consuming it.
  const Token& Peek();

  /// Byte offset → (1-based line, 1-based column), for diagnostics.
  void GetLineCol(uint32_t offset, uint32_t& line, uint32_t& col) const;

 private:
  Token Lex();
  Token LexIdentifier();
  Token LexRegister();
  Token LexNumber();
  Token LexString();
  Token LexChar();
  Token Make(TokKind kind, uint32_t start);
  Token Error(uint32_t start, std::string msg);

  int Peek0() const { return pos_ < src_.size() ? (unsigned char)src_[pos_] : -1; }
  int Peek1() const {
    return pos_ + 1 < src_.size() ? (unsigned char)src_[pos_ + 1] : -1;
  }
  void SkipHorizontalWs();

  std::string_view src_;
  size_t pos_ = 0;
  bool has_peek_ = false;
  Token peeked_;
};

}  // namespace bcc::as
