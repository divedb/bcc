#include "bcc/lex/literal_support.hh"

#include <cctype>
#include <cstdlib>

#include "bcc/basic/diagnostic.hh"

namespace bcc {

namespace {

bool IsHexDigit(char c) {
  return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}
bool IsOctalDigit(char c) { return c >= '0' && c <= '7'; }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }

unsigned HexValue(char c) {
  if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
  return static_cast<unsigned>(c - 'A' + 10);
}

/// Decodes one escape sequence starting at \p i (which indexes the char after
/// the backslash). Returns the value and advances \p i past the sequence.
/// \p max_value is the largest value representable in the literal's element
/// type (used to diagnose out-of-range hex/octal escapes).
uint32_t DecodeEscape(std::string_view s, std::size_t& i, uint64_t max_value,
                      SourceLocation loc, DiagnosticsEngine& diags,
                      bool& had_error) {
  char c = s[i++];
  switch (c) {
    case '\'': return '\'';
    case '"': return '"';
    case '?': return '?';
    case '\\': return '\\';
    case 'a': return '\a';
    case 'b': return '\b';
    case 'f': return '\f';
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    case 'v': return '\v';
    case 'x': {
      uint64_t value = 0;
      bool overflow = false;
      if (i >= s.size() || !IsHexDigit(s[i])) {
        diags.Report(loc, diag::err_unknown_escape) << "x";
        had_error = true;
        return 0;
      }
      while (i < s.size() && IsHexDigit(s[i])) {
        value = value * 16 + HexValue(s[i++]);
        if (value > 0xFFFF'FFFFull) overflow = true;
      }
      if (overflow || value > max_value) {
        diags.Report(loc, diag::err_escape_too_large) << "hex";
        had_error = true;
        value &= max_value;
      }
      return static_cast<uint32_t>(value);
    }
    case 'u':
    case 'U': {
      unsigned num_digits = c == 'u' ? 4 : 8;
      uint32_t value = 0;
      for (unsigned d = 0; d < num_digits; ++d) {
        if (i >= s.size() || !IsHexDigit(s[i])) {
          had_error = true;
          return value;
        }
        value = value * 16 + HexValue(s[i++]);
      }
      return value;
    }
    default:
      if (IsOctalDigit(c)) {
        uint32_t value = static_cast<uint32_t>(c - '0');
        // Up to three octal digits total.
        for (unsigned d = 0; d < 2 && i < s.size() && IsOctalDigit(s[i]);
             ++d) {
          value = value * 8 + static_cast<uint32_t>(s[i++] - '0');
        }
        if (value > max_value) {
          diags.Report(loc, diag::err_escape_too_large) << "octal";
          had_error = true;
          value &= static_cast<uint32_t>(max_value);
        }
        return value;
      }
      diags.Report(loc, diag::err_unknown_escape) << std::string(1, c);
      had_error = true;
      return static_cast<uint32_t>(c);
  }
}

/// Encodes \p code_point as UTF-8 into \p out.
void AppendUTF8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

void AppendElement(std::string& out, uint32_t value, unsigned width) {
  for (unsigned b = 0; b < width; ++b) {
    out += static_cast<char>((value >> (8 * b)) & 0xFF);
  }
}

}  // namespace

//===----------------------------------------------------------------------===//
// NumericLiteralParser
//===----------------------------------------------------------------------===//

NumericLiteralParser::NumericLiteralParser(std::string_view spelling,
                                           SourceLocation loc,
                                           DiagnosticsEngine& diags)
    : spelling_(spelling), loc_(loc), diags_(diags) {
  std::size_t i = 0;
  std::size_t n = spelling.size();

  auto skip_digits = [&](auto pred) {
    while (i < n && pred(spelling_[i])) ++i;
  };

  if (spelling_[0] == '0' && n > 1) {
    char second = spelling_[1];
    if (second == 'x' || second == 'X') {
      radix_ = 16;
      i = 2;
      digits_begin_ = i;
      skip_digits(IsHexDigit);
      if (i == digits_begin_ && (i >= n || spelling_[i] != '.')) {
        diags_.Report(loc_, diag::err_invalid_digit)
            << std::string(1, i < n ? spelling_[i] : '?') << "hexadecimal";
        had_error_ = true;
        return;
      }
      // Hex float: 0x1.8p3
      if (i < n && spelling_[i] == '.') {
        is_floating_ = true;
        ++i;
        skip_digits(IsHexDigit);
      }
      if (i < n && (spelling_[i] == 'p' || spelling_[i] == 'P')) {
        is_floating_ = true;
        ++i;
        if (i < n && (spelling_[i] == '+' || spelling_[i] == '-')) ++i;
        if (i >= n || !IsDigit(spelling_[i])) {
          diags_.Report(loc_, diag::err_exponent_has_no_digits);
          had_error_ = true;
          return;
        }
        skip_digits(IsDigit);
      } else if (is_floating_) {
        diags_.Report(loc_, diag::err_hex_constant_requires_exponent);
        had_error_ = true;
        return;
      }
    } else if (second == 'b' || second == 'B') {
      // GNU binary literal.
      radix_ = 2;
      i = 2;
      digits_begin_ = i;
      skip_digits([](char c) { return c == '0' || c == '1'; });
      if (i == digits_begin_) {
        diags_.Report(loc_, diag::err_invalid_digit)
            << std::string(1, i < n ? spelling_[i] : '?') << "binary";
        had_error_ = true;
        return;
      }
    } else {
      // Octal — unless it turns out to be a floating literal like 0.5 or
      // 09.0, which is decimal.
      radix_ = 8;
      digits_begin_ = i;
      skip_digits(IsDigit);
      std::size_t digits_end = i;
      if (i < n &&
          (spelling_[i] == '.' || spelling_[i] == 'e' || spelling_[i] == 'E')) {
        radix_ = 10;
        goto decimal_fraction;
      }
      // Validate that all digits were octal.
      for (std::size_t d = digits_begin_; d < digits_end; ++d) {
        if (!IsOctalDigit(spelling_[d])) {
          diags_.Report(loc_, diag::err_invalid_digit)
              << std::string(1, spelling_[d]) << "octal";
          had_error_ = true;
          return;
        }
      }
    }
  } else {
    radix_ = 10;
    digits_begin_ = i;
    skip_digits(IsDigit);
  decimal_fraction:
    if (i < n && spelling_[i] == '.') {
      is_floating_ = true;
      ++i;
      skip_digits(IsDigit);
    }
    if (i < n && (spelling_[i] == 'e' || spelling_[i] == 'E')) {
      is_floating_ = true;
      ++i;
      if (i < n && (spelling_[i] == '+' || spelling_[i] == '-')) ++i;
      if (i >= n || !IsDigit(spelling_[i])) {
        diags_.Report(loc_, diag::err_exponent_has_no_digits);
        had_error_ = true;
        return;
      }
      skip_digits(IsDigit);
    }
  }

  suffix_begin_ = i;
  if (!ParseSuffix(spelling_.substr(suffix_begin_))) {
    diags_.Report(loc_, diag::err_invalid_suffix_constant)
        << std::string(spelling_.substr(suffix_begin_))
        << (is_floating_ ? "floating" : "integer");
    had_error_ = true;
  }
}

bool NumericLiteralParser::ParseSuffix(std::string_view suffix) {
  bool seen_unsigned = false;
  bool seen_long = false;
  bool seen_float = false;
  std::size_t i = 0;
  while (i < suffix.size()) {
    char c = suffix[i];
    if ((c == 'u' || c == 'U') && !seen_unsigned && !is_floating_) {
      seen_unsigned = true;
      is_unsigned_ = true;
      ++i;
    } else if ((c == 'l' || c == 'L') && !seen_long) {
      seen_long = true;
      if (i + 1 < suffix.size() && suffix[i + 1] == c && !is_floating_) {
        is_long_long_ = true;
        i += 2;
      } else {
        is_long_ = true;
        ++i;
      }
    } else if ((c == 'f' || c == 'F') && is_floating_ && !seen_float &&
               !seen_long) {
      seen_float = true;
      is_float_suffix_ = true;
      ++i;
    } else {
      return false;
    }
  }
  return true;
}

bool NumericLiteralParser::GetIntegerValue(uint64_t& value) const {
  value = 0;
  bool overflow = false;
  for (std::size_t i = digits_begin_; i < suffix_begin_; ++i) {
    char c = spelling_[i];
    unsigned digit;
    if (IsDigit(c)) {
      digit = static_cast<unsigned>(c - '0');
    } else {
      digit = HexValue(c);
    }
    uint64_t next = value * radix_ + digit;
    if (value != 0 && (next - digit) / radix_ != value) overflow = true;
    if (next < digit) overflow = true;
    value = next;
  }
  return overflow;
}

double NumericLiteralParser::GetFloatValue() const {
  std::string text(spelling_.substr(0, suffix_begin_));
  return std::strtod(text.c_str(), nullptr);
}

//===----------------------------------------------------------------------===//
// CharLiteralParser
//===----------------------------------------------------------------------===//

CharLiteralParser::CharLiteralParser(std::string_view spelling,
                                     SourceLocation loc, TokenKind kind,
                                     DiagnosticsEngine& diags) {
  // Strip the prefix (L, u, U) and the quotes.
  std::size_t begin = 0;
  while (begin < spelling.size() && spelling[begin] != '\'') ++begin;
  std::size_t end = spelling.rfind('\'');
  if (begin >= end || end == std::string_view::npos) {
    had_error_ = true;
    return;
  }
  std::string_view body = spelling.substr(begin + 1, end - begin - 1);
  if (body.empty()) {
    diags.Report(loc, diag::err_empty_character);
    had_error_ = true;
    return;
  }

  uint64_t max_value = kind == TokenKind::kCharConstant ? 0xFF : 0xFFFF'FFFFull;

  std::vector<uint32_t> chars;
  std::size_t i = 0;
  while (i < body.size()) {
    if (body[i] == '\\') {
      ++i;
      chars.push_back(
          DecodeEscape(body, i, max_value, loc, diags, had_error_));
    } else {
      chars.push_back(static_cast<unsigned char>(body[i++]));
    }
  }

  if (chars.size() == 1) {
    value_ = chars[0];
    // A plain char constant is sign-extended through char on x86-64:
    // '\xFF' == -1.
    if (kind == TokenKind::kCharConstant && value_ > 0x7F && value_ <= 0xFF) {
      value_ = static_cast<uint32_t>(static_cast<int8_t>(value_));
    }
    return;
  }

  // Multi-character constant: 'ab' has type int, value implementation-
  // defined; match Clang/GCC by packing bytes big-endian-wise.
  is_multi_char_ = true;
  if (kind == TokenKind::kCharConstant) {
    diags.Report(loc, diag::warn_multichar_character_constant);
    uint32_t v = 0;
    for (uint32_t c : chars) v = (v << 8) | (c & 0xFF);
    value_ = v;
  } else {
    // Wide multi-char: value is the last character (Clang's behavior).
    value_ = chars.back();
  }
}

//===----------------------------------------------------------------------===//
// StringLiteralParser
//===----------------------------------------------------------------------===//

StringLiteralParser::StringLiteralParser(const std::vector<Piece>& pieces,
                                         DiagnosticsEngine& diags) {
  // Determine the element kind of the concatenation (C11 6.4.5p5): if any
  // piece has an encoding prefix, the result has that prefix; mixing distinct
  // non-plain prefixes is a constraint violation.
  for (const Piece& p : pieces) {
    if (p.kind == TokenKind::kStringLiteral) continue;
    if (kind_ != TokenKind::kStringLiteral && kind_ != p.kind) {
      diags.Report(p.loc, diag::err_unsupported_string_concat);
      had_error_ = true;
      return;
    }
    kind_ = p.kind;
  }

  switch (kind_) {
    case TokenKind::kStringLiteral:
    case TokenKind::kUtf8StringLiteral:
      char_byte_width_ = 1;
      break;
    case TokenKind::kUtf16StringLiteral:
      char_byte_width_ = 2;
      break;
    case TokenKind::kUtf32StringLiteral:
    case TokenKind::kWideStringLiteral:  // wchar_t is 4 bytes on x86-64
      char_byte_width_ = 4;
      break;
    default:
      break;
  }

  for (const Piece& p : pieces) {
    std::string_view s = p.spelling;
    std::size_t begin = s.find('"');
    std::size_t end = s.rfind('"');
    if (begin == std::string_view::npos || begin >= end) {
      had_error_ = true;
      return;
    }
    std::string_view body = s.substr(begin + 1, end - begin - 1);
    uint64_t max_value =
        char_byte_width_ == 1 ? 0xFF
                              : (char_byte_width_ == 2 ? 0xFFFF : 0xFFFF'FFFF);

    std::size_t i = 0;
    while (i < body.size()) {
      if (body[i] == '\\') {
        ++i;
        char esc = i < body.size() ? body[i] : 0;
        uint32_t value =
            DecodeEscape(body, i, max_value, p.loc, diags, had_error_);
        if ((esc == 'u' || esc == 'U') && char_byte_width_ == 1) {
          AppendUTF8(bytes_, value);  // UCN in a narrow string -> UTF-8
        } else {
          AppendElement(bytes_, value, char_byte_width_);
        }
      } else if (char_byte_width_ == 1) {
        bytes_ += body[i++];
      } else {
        // Wide string: source bytes are ASCII/UTF-8; store each byte as one
        // element (sufficient for ASCII; full UTF-8 decoding is future work).
        AppendElement(bytes_, static_cast<unsigned char>(body[i++]),
                      char_byte_width_);
      }
    }
  }
}

}  // namespace bcc
