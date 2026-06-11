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

bool IsValidCodepoint(uint32_t codepoint, int encoded_len) noexcept {
  const int required_len = codepoint < 0x0080    ? 1
                           : codepoint < 0x0800  ? 2
                           : codepoint < 0x10000 ? 3
                                                 : 4;

  return encoded_len == required_len &&
         (codepoint < 0xD800 || codepoint > 0xDFFF) && codepoint <= 0x10FFFF;
}

}  // namespace

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
//
// On invalid UTF-8 the cursor is placed exactly one byte past the lead byte
// of the bad sequence. Any line splices consumed while trying to decode the
// continuation bytes are rolled back along with the cursor, because they
// logically belong to the sequence that was rejected.
DecodedChar Cursor::Next() noexcept {
  had_line_splice_ |= SkipLineSplice();

  if (current_ >= end_) return {DecodedChar::kEOF};

  const char* const lead_pos = current_;
  const bool splice_before_lead = had_line_splice_;

  // On any invalid UTF-8: place cursor one past the lead byte and roll back
  // any splice flags picked up while trying to decode continuation bytes.
  auto reject = [&]() -> DecodedChar {
    current_ = lead_pos + 1;
    had_line_splice_ = splice_before_lead;

    return {DecodedChar::kInvalid};
  };

  const auto lead = static_cast<unsigned char>(*current_);
  const int len = kUtf8ByteLength[lead];

  // Lone continuation byte (0x80–0xBF) or not enough bytes remain.
  if (len == 0 || current_ + len > end_) return reject();

  uint32_t codepoint = lead & ((1 << (8 - len)) - 1);
  ++current_;

  for (int i = 1; i < len; ++i) {
    had_line_splice_ |= SkipLineSplice();

    if (current_ >= end_) return reject();

    const auto byte = static_cast<unsigned char>(*current_);

    if ((byte & 0xC0) != 0x80) return reject();

    codepoint = (codepoint << 6) | (byte & 0x3F);
    ++current_;
  }

  // Overlong encoding, surrogate, or out-of-range code point.
  if (!IsValidCodepoint(codepoint, len)) return reject();

  return {codepoint};
}

}  // namespace bcc
