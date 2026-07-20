#pragma once

#include <string_view>

#include "bcc/basic/source_location.hh"
#include "bcc/lex/token_kind.hh"

namespace bcc {

class IdentifierInfo;

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
  /// \brief Constructs an invalid token with no source location, no lexeme, and
  ///        kUnknown kind.
  Token() noexcept
      : data_(nullptr),
        loc_(),
        length_(0),
        ident_(nullptr),
        kind_(TokenKind::kUnknown),
        flag_(TokenFlag::kNone) {}

  /// \brief Constructs a token with the given properties.
  ///
  /// \param loc    The source location of the first byte of this token's raw
  ///               source text.
  /// \param kind   The token kind.
  /// \param data   Pointer into the owning SourceManager's buffer at the first
  ///               byte of this token's raw source text. Not null-terminated.
  /// \param length Number of raw source bytes, including any backslash-newline
  ///               sequences (see NeedsCleaning()).
  /// \param flag   Initial token flags.
  Token(SourceLocation loc, TokenKind kind, const char* data, uint32_t length,
        TokenFlag flag = TokenFlag::kNone)
      : data_(data), loc_(loc), length_(length), kind_(kind), flag_(flag) {}

  SourceLocation GetLocation() const noexcept { return loc_; }
  TokenKind GetKind() const noexcept { return kind_; }

  bool IsIdentifier() const noexcept { return kind_ == TokenKind::kIdentifier; }

  /// \brief Checks if the token kind matches any of the specified kinds.
  ///
  /// \param kinds The token kinds to check against.
  /// \return      True if the token kind matches any of the specified
  ///              kinds; otherwise false.
  bool IsOneOf(std::initializer_list<TokenKind> kinds) const noexcept {
    for (TokenKind k : kinds) {
      if (kind_ == k) return true;
    }

    return false;
  }

  /// \brief Overwrites the token kind. Used by the preprocessor to promote an
  ///        identifier token to its keyword kind after identifier lookup.
  ///
  /// \param kind The new token kind.
  void SetKind(TokenKind kind) noexcept { kind_ = kind; }

  /// \brief Overwrites the source location. Used by the TokenLexer to relocate
  ///        a macro's replacement tokens into their macro-expansion slots.
  ///
  /// \param loc The new source location.
  void SetLocation(SourceLocation loc) noexcept { loc_ = loc; }

  void SetFlag(TokenFlag flag) noexcept { flag_ |= flag; }

  /// \brief Clears the given token flag.
  ///
  /// \param flag The token flag to clear.
  void ClearFlag(TokenFlag flag) noexcept {
    flag_ = static_cast<TokenFlag>(static_cast<uint8_t>(flag_) &
                                   ~static_cast<uint8_t>(flag));
  }

  /// \brief The interned identifier information for this token, or nullptr.
  ///
  /// Set by the preprocessor (see Preprocessor::ResolveIdentifier) for
  /// identifier and keyword tokens; nullptr for all other tokens and for any
  /// identifier that has not yet been looked up.
  ///
  /// \return The interned identifier information for this token, or nullptr.
  IdentifierInfo* GetIdentifierInfo() const noexcept { return ident_; }
  void SetIdentifierInfo(IdentifierInfo* info) noexcept { ident_ = info; }

  /// \brief Returns a view of the raw source bytes for this token. The view is
  ///        valid for the lifetime of the owning SourceManager. If
  ///        NeedsCleaning() is true, backslash-newline sequences must be
  ///        stripped before the text is used.
  ///
  /// \return A view of the raw source bytes for this token.
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

  /// The source location of the first byte of this token's raw source text.
  SourceLocation loc_;

  /// Pointer into the owning SourceManager's buffer at the first byte of this
  /// token's raw source text. Not null-terminated.
  const char* data_;

  /// The number of raw source bytes, including any backslash-newline sequences
  /// (see NeedsCleaning()).
  uint32_t length_;

  /// The interned identifier information for this token, or nullptr.
  IdentifierInfo* ident_ = nullptr;

  TokenKind kind_;
  TokenFlag flag_;
};

}  // namespace bcc
