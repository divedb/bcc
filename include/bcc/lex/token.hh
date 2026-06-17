#pragma once

#include <string_view>

#include "bcc/basic/source_location.hh"
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
  /// \param data   Pointer into the owning SourceManager's buffer at the first
  ///               byte of this token's raw source text. Not null-terminated.
  /// \param length Number of raw source bytes, including any backslash-newline
  ///               sequences (see NeedsCleaning()).
  Token(SourceLocation loc, TokenKind kind, const char* data, uint32_t length,
        TokenFlag flag = TokenFlag::kNone)
      : data_(data), loc_(loc), length_(length), kind_(kind), flag_(flag) {}

  SourceLocation GetLocation() const noexcept { return loc_; }
  TokenKind GetKind() const noexcept { return kind_; }

  /// Returns a view of the raw source bytes for this token. The view is valid
  /// for the lifetime of the owning SourceManager. If NeedsCleaning() is true,
  /// backslash-newline sequences must be stripped before the text is used.
  std::string_view GetLexeme() const noexcept { return {data_, length_}; }

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

  SourceLocation loc_;  // global offset of data_[0]
  const char* data_;    // non-owning pointer into SourceManager's buffer
  uint32_t length_;
  TokenKind kind_;
  TokenFlag flag_;
};

}  // namespace bcc
