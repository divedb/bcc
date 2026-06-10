#pragma once

#include <cassert>
#include <cctype>

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

}  // namespace bcc