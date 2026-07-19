#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "bcc/basic/source_location.hh"
#include "bcc/lex/token_kind.hh"

namespace bcc {

class DiagnosticsEngine;

/// \brief Decodes the spelling of a numeric-constant token (C11 6.4.4.1/2):
///        radix, digits, and suffixes. Mirrors Clang's NumericLiteralParser.
///
/// The spelling must already be cleaned of escaped newlines.
class NumericLiteralParser {
 public:
  NumericLiteralParser(std::string_view spelling, SourceLocation loc,
                       DiagnosticsEngine& diags);

  bool HadError() const noexcept { return had_error_; }
  bool IsFloatingLiteral() const noexcept { return is_floating_; }
  unsigned GetRadix() const noexcept { return radix_; }

  bool IsUnsigned() const noexcept { return is_unsigned_; }
  bool IsLong() const noexcept { return is_long_; }
  bool IsLongLong() const noexcept { return is_long_long_; }
  bool HasFloatSuffix() const noexcept { return is_float_suffix_; }

  /// \brief Evaluates the integer value. Returns true on overflow of uint64.
  bool GetIntegerValue(uint64_t& value) const;

  /// \brief Evaluates a floating literal's value.
  double GetFloatValue() const;

 private:
  void ParseNumberStartingWithZero();
  bool ParseSuffix(std::string_view suffix);

  std::string_view spelling_;
  SourceLocation loc_;
  DiagnosticsEngine& diags_;

  std::size_t digits_begin_ = 0;  // offset of first digit
  std::size_t suffix_begin_ = 0;  // offset just past digits/exponent

  unsigned radix_ = 10;
  bool is_floating_ = false;
  bool is_unsigned_ = false;
  bool is_long_ = false;
  bool is_long_long_ = false;
  bool is_float_suffix_ = false;
  bool had_error_ = false;
};

/// \brief Decodes a character-constant token, including escape sequences and
///        multi-character constants. Mirrors Clang's CharLiteralParser.
class CharLiteralParser {
 public:
  /// \param spelling The full cleaned spelling including quotes and prefix.
  /// \param kind     One of the kCharConstant token kinds.
  CharLiteralParser(std::string_view spelling, SourceLocation loc,
                    TokenKind kind, DiagnosticsEngine& diags);

  bool HadError() const noexcept { return had_error_; }
  bool IsMultiChar() const noexcept { return is_multi_char_; }
  uint32_t GetValue() const noexcept { return value_; }

 private:
  uint32_t value_ = 0;
  bool is_multi_char_ = false;
  bool had_error_ = false;
};

/// \brief Concatenates and decodes a sequence of adjacent string-literal
///        tokens (C11 6.4.5, 5.1.1.2p6). Mirrors Clang's StringLiteralParser.
class StringLiteralParser {
 public:
  struct Piece {
    std::string spelling;  // cleaned, including quotes and prefix
    SourceLocation loc;
    TokenKind kind;
  };

  StringLiteralParser(const std::vector<Piece>& pieces,
                      DiagnosticsEngine& diags);

  bool HadError() const noexcept { return had_error_; }

  /// The element kind after concatenation (plain/utf8/utf16/utf32/wide).
  TokenKind GetKind() const noexcept { return kind_; }

  /// Bytes per element: 1 (char/u8), 2 (u), or 4 (U/L on x86-64).
  unsigned GetCharByteWidth() const noexcept { return char_byte_width_; }

  /// The decoded bytes (elements stored little-endian), without the NUL.
  const std::string& GetBytes() const noexcept { return bytes_; }

  /// Number of elements, excluding the terminating NUL.
  uint64_t GetNumElements() const noexcept {
    return bytes_.size() / char_byte_width_;
  }

 private:
  std::string bytes_;
  TokenKind kind_ = TokenKind::kStringLiteral;
  unsigned char_byte_width_ = 1;
  bool had_error_ = false;
};

}  // namespace bcc
