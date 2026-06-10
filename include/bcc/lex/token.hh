#pragma once

#include <string>

#include "bcc/lex/source_location.hh"
#include "bcc/lex/token_kind.hh"

namespace bcc {

enum class TokenFlag : uint8_t {
  kNone = 0,
  kStartOfLine = 0x01,    ///< At start of line or only after whitespace.
  kLeadingSpace = 0x02,   ///< Whitespace exists before this token.
  kDisableExpand = 0x04,  ///< This identifier may never be macro expanded.
  kNeedsCleaning = 0x08   ///< Contained an escaped newline.
};

inline constexpr TokenFlag operator|(TokenFlag lhs, TokenFlag rhs) {
  return static_cast<TokenFlag>(static_cast<uint8_t>(lhs) |
                                static_cast<uint8_t>(rhs));
}

inline constexpr TokenFlag operator&(TokenFlag lhs, TokenFlag rhs) {
  return static_cast<TokenFlag>(static_cast<uint8_t>(lhs) &
                                static_cast<uint8_t>(rhs));
}

inline constexpr TokenFlag& operator|=(TokenFlag& lhs, TokenFlag rhs) {
  return lhs = lhs | rhs;
}

class Token {
 public:
  Token(SourceLocation loc, TokenKind kind, std::string lexeme,
        TokenFlag flag = TokenFlag::kNone)
      : loc_(loc), kind_(kind), flag_(flag), lexeme_(std::move(lexeme)) {}

  SourceLocation GetLocation() const noexcept { return loc_; }
  TokenKind GetKind() const noexcept { return kind_; }

  const std::string& GetLexeme() const noexcept { return lexeme_; }

  bool IsStartOfLine() const noexcept {
    return HasFlag(TokenFlag::kStartOfLine);
  }

  bool HasLeadingSpace() const noexcept {
    return HasFlag(TokenFlag::kLeadingSpace);
  }

  bool IsDisableExpand() const noexcept {
    return HasFlag(TokenFlag::kDisableExpand);
  }

  bool NeedsCleaning() const noexcept {
    return HasFlag(TokenFlag::kNeedsCleaning);
  }

 private:
  bool HasFlag(TokenFlag flag) const noexcept {
    return (flag_ & flag) != TokenFlag::kNone;
  }

  SourceLocation loc_;
  TokenKind kind_;
  TokenFlag flag_;
  std::string lexeme_;
};

}  // namespace bcc
