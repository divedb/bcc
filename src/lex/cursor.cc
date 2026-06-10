#include "bcc/lex/cursor.hh"

namespace bcc {

namespace {

inline constexpr int kUtf8ByteLength[256] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  //
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  //
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  //
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  //
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  //
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  //
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  //
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,  //
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  //
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  //
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  //
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  //
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,  //
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,  //
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,  //
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,  //
};

}

// UTF-8 encodes a Unicode code point using one to four bytes:
//
//   U+0000 ..U+007F     0xxxxxxx
//   U+0080 ..U+07FF     110xxxxx 10xxxxxx
//   U+0800 ..U+FFFF     1110xxxx 10xxxxxx 10xxxxxx
//   U+10000..U+10FFFF   11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
//
// Examples:
//
//   'A'    U+0041  => 41
//   '¢'    U+00A2  => C2 A2
//   '€'    U+20AC  => E2 82 AC
//   '𐍈'    U+10348 => F0 90 8D 88
DecodedChar Cursor::DecodeUTF8() noexcept {
  constexpr uint8_t kUtf8ContinuationMask = 0xC0;       // 1100'0000
  constexpr uint8_t kUtf8ContinuationBits = 0x80;       // 1000'0000
  constexpr uint8_t kUtf8ContinuationValueMask = 0x3F;  // 0011'1111

  // Each decode step observes the logical character stream after phase-2
  // backslash-newline deletion, including between UTF-8 continuation bytes.
  had_line_splice_ |= SkipLineSplice();

  if (current_ >= end_) return {DecodedChar::kEOF};

  const auto lead = static_cast<unsigned char>(*current_);
  const int len = kUtf8ByteLength[lead];

  if (len == 0 || current_ + len > end_) return {DecodedChar::kInvalid};

  uint32_t codepoint = lead & ((1 << (8 - len)) - 1);
  ++current_;

  for (int i = 1; i < len; ++i) {
    had_line_splice_ |= SkipLineSplice();

    if (current_ >= end_) return {DecodedChar::kEOF};

    const auto byte = static_cast<unsigned char>(*current_);

    if ((byte & kUtf8ContinuationMask) != kUtf8ContinuationBits) {
      return {DecodedChar::kInvalid};
    }

    codepoint = (codepoint << 6) | (byte & kUtf8ContinuationValueMask);
    ++current_;
  }

  return {codepoint};
}

}  // namespace bcc
