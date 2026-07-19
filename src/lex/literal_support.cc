#include "bcc/lex/literal_support.hh"

#include <cctype>

#include "bcc/basic/diagnostic.hh"

namespace bcc {
namespace {

bool IsHexDigit(char c) {
  return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

bool IsOctalDigit(char c) { return c >= '0' && c <= '7'; }

unsigned HexValue(char c) {
  if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
  return static_cast<unsigned>(c - 'A' + 10);
}

uint32_t DecodeEscape(std::string_view text, std::size_t& pos,
                      uint64_t max_value, SourceLocation loc,
                      DiagnosticsEngine& diags, bool& had_error) {
  if (pos == text.size()) {
    diags.Report(loc, diag::err_unknown_escape) << "";
    had_error = true;
    return 0;
  }

  char c = text[pos++];
  switch (c) {
    case '\'':
      return '\'';
    case '"':
      return '"';
    case '?':
      return '?';
    case '\\':
      return '\\';
    case 'a':
      return '\a';
    case 'b':
      return '\b';
    case 'f':
      return '\f';
    case 'n':
      return '\n';
    case 'r':
      return '\r';
    case 't':
      return '\t';
    case 'v':
      return '\v';
    case 'x': {
      uint64_t value = 0;
      bool overflow = false;
      if (pos == text.size() || !IsHexDigit(text[pos])) {
        diags.Report(loc, diag::err_unknown_escape) << "x";
        had_error = true;
        return 0;
      }
      while (pos < text.size() && IsHexDigit(text[pos])) {
        value = value * 16 + HexValue(text[pos++]);
        overflow |= value > 0xFFFF'FFFFull;
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
      unsigned digit_count = c == 'u' ? 4 : 8;
      uint32_t value = 0;
      for (unsigned digit = 0; digit < digit_count; ++digit) {
        if (pos == text.size() || !IsHexDigit(text[pos])) {
          diags.Report(loc, diag::err_unknown_escape) << std::string(1, c);
          had_error = true;
          return value;
        }
        value = value * 16 + HexValue(text[pos++]);
      }
      return value;
    }
    default:
      if (IsOctalDigit(c)) {
        uint32_t value = static_cast<uint32_t>(c - '0');
        for (unsigned digit = 1;
             digit < 3 && pos < text.size() && IsOctalDigit(text[pos]); ++digit)
          value = value * 8 + static_cast<uint32_t>(text[pos++] - '0');
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

void AppendUTF8(std::string& out, uint32_t code_point) {
  if (code_point < 0x80) {
    out += static_cast<char>(code_point);
  } else if (code_point < 0x800) {
    out += static_cast<char>(0xC0 | (code_point >> 6));
    out += static_cast<char>(0x80 | (code_point & 0x3F));
  } else if (code_point < 0x10000) {
    out += static_cast<char>(0xE0 | (code_point >> 12));
    out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code_point & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (code_point >> 18));
    out += static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code_point & 0x3F));
  }
}

void AppendElement(std::string& out, uint32_t value, unsigned width) {
  for (unsigned byte = 0; byte < width; ++byte)
    out += static_cast<char>((value >> (8 * byte)) & 0xFF);
}

}  // namespace

CharLiteralParser::CharLiteralParser(std::string_view spelling,
                                     SourceLocation loc, TokenKind kind,
                                     DiagnosticsEngine& diags) {
  std::size_t begin = spelling.find('\'');
  std::size_t end = spelling.rfind('\'');
  if (begin == std::string_view::npos || begin >= end) {
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
  for (std::size_t pos = 0; pos < body.size();) {
    if (body[pos] == '\\') {
      ++pos;
      chars.push_back(
          DecodeEscape(body, pos, max_value, loc, diags, had_error_));
    } else {
      chars.push_back(static_cast<unsigned char>(body[pos++]));
    }
  }

  if (chars.size() == 1) {
    value_ = chars.front();
    if (kind == TokenKind::kCharConstant && value_ > 0x7F && value_ <= 0xFF)
      value_ = static_cast<uint32_t>(static_cast<int8_t>(value_));
    return;
  }

  is_multi_char_ = true;
  if (kind == TokenKind::kCharConstant) {
    diags.Report(loc, diag::warn_multichar_character_constant);
    for (uint32_t c : chars) value_ = (value_ << 8) | (c & 0xFF);
  } else {
    value_ = chars.back();
  }
}

StringLiteralParser::StringLiteralParser(const std::vector<Piece>& pieces,
                                         DiagnosticsEngine& diags) {
  for (const Piece& piece : pieces) {
    if (piece.kind == TokenKind::kStringLiteral) continue;
    if (kind_ != TokenKind::kStringLiteral && kind_ != piece.kind) {
      diags.Report(piece.loc, diag::err_unsupported_string_concat);
      had_error_ = true;
      return;
    }
    kind_ = piece.kind;
  }

  switch (kind_) {
    case TokenKind::kUtf16StringLiteral:
      char_byte_width_ = 2;
      break;
    case TokenKind::kUtf32StringLiteral:
    case TokenKind::kWideStringLiteral:
      char_byte_width_ = 4;
      break;
    default:
      char_byte_width_ = 1;
      break;
  }

  for (const Piece& piece : pieces) {
    std::size_t begin = piece.spelling.find('"');
    std::size_t end = piece.spelling.rfind('"');
    if (begin == std::string::npos || begin >= end) {
      had_error_ = true;
      return;
    }

    std::string_view body(piece.spelling.data() + begin + 1, end - begin - 1);
    uint64_t max_value = char_byte_width_ == 1   ? 0xFF
                         : char_byte_width_ == 2 ? 0xFFFF
                                                 : 0xFFFF'FFFF;
    for (std::size_t pos = 0; pos < body.size();) {
      if (body[pos] == '\\') {
        ++pos;
        char escape = pos < body.size() ? body[pos] : 0;
        uint32_t value =
            DecodeEscape(body, pos, max_value, piece.loc, diags, had_error_);
        if ((escape == 'u' || escape == 'U') && char_byte_width_ == 1)
          AppendUTF8(bytes_, value);
        else
          AppendElement(bytes_, value, char_byte_width_);
      } else if (char_byte_width_ == 1) {
        bytes_ += body[pos++];
      } else {
        AppendElement(bytes_, static_cast<unsigned char>(body[pos++]),
                      char_byte_width_);
      }
    }
  }
}

}  // namespace bcc
