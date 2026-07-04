#pragma once

#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>

namespace bcc {

constexpr bool IsSign(uint32_t cp) noexcept { return cp == '+' || cp == '-'; }

constexpr bool IsNewLine(uint32_t cp) noexcept {
  return cp == '\n' || cp == '\r';
}

constexpr bool IsDigit(uint32_t cp) noexcept { return cp >= '0' && cp <= '9'; }

constexpr bool IsHexDigit(uint32_t cp) noexcept {
  return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'f') ||
         (cp >= 'A' && cp <= 'F');
}

constexpr int HexDigitValue(uint32_t cp) noexcept {
  assert(IsHexDigit(cp) && "Expected a hex digit");

  if (cp >= '0' && cp <= '9') return cp - '0';
  if (cp >= 'a' && cp <= 'f') return cp - 'a' + 10;
  if (cp >= 'A' && cp <= 'F') return cp - 'A' + 10;

  __builtin_unreachable();
}

// Appends the UTF-8 encoding of codepoint \p cp to \p out.
inline void AppendUtf8(uint32_t cp, std::string& out) {
  if (cp <= 0x7F) {
    out += static_cast<char>(cp);
  } else if (cp <= 0x7FF) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp <= 0xFFFF) {
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

// Decodes \uXXXX and \UXXXXXXXX universal-character-names in an identifier's
// spelling to their UTF-8 encoding, matching how Clang prints identifiers in
// -E output. Other backslash sequences are left untouched.
inline std::string DecodeIdentifierUCNs(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    if (s[i] == '\\' && i + 1 < s.size() &&
        (s[i + 1] == 'u' || s[i + 1] == 'U')) {
      int digits = (s[i + 1] == 'u') ? 4 : 8;
      std::size_t j = i + 2;
      uint32_t cp = 0;
      bool ok = true;
      for (int k = 0; k < digits; ++k) {
        if (j + k >= s.size() || !IsHexDigit(static_cast<unsigned char>(s[j + k]))) {
          ok = false;
          break;
        }
        cp = (cp << 4) + HexDigitValue(static_cast<unsigned char>(s[j + k]));
      }
      if (ok) {
        AppendUtf8(cp, out);
        i = j + digits;
        continue;
      }
    }
    out += s[i++];
  }
  return out;
}

}  // namespace bcc