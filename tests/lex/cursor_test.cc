#include "bcc/lex/cursor.hh"

#include <string>

#include "gtest/gtest.h"

namespace bcc {
namespace {

// Build a string from a list of raw byte values.
std::string Bytes(std::initializer_list<unsigned char> bytes) {
  std::string s;
  s.reserve(bytes.size());

  for (auto b : bytes) s += static_cast<char>(b);

  return s;
}

// Decode the first code point from `input` and return it.
DecodedChar DecodeFirst(const std::string& input) {
  Cursor c(input);

  return c.Next();
}

//=== Baseline: valid sequences that must keep working ===//

TEST(CursorTryDecodeUTF8Test, ValidAscii) {
  EXPECT_EQ(DecodeFirst("A").codepoint, 0x0041u);
}

TEST(CursorTryDecodeUTF8Test, ValidTwoByte) {
  // U+00A2 CENT SIGN  -> 0xC2 0xA2
  EXPECT_EQ(DecodeFirst(Bytes({0xC2, 0xA2})).codepoint, 0x00A2u);
}

TEST(CursorTryDecodeUTF8Test, ValidThreeByte) {
  // U+20AC EURO SIGN  -> 0xE2 0x82 0xAC
  EXPECT_EQ(DecodeFirst(Bytes({0xE2, 0x82, 0xAC})).codepoint, 0x20ACu);
}

TEST(CursorTryDecodeUTF8Test, ValidFourByte) {
  // U+10348 GOTHIC LETTER HWAIR  -> 0xF0 0x90 0x8D 0x88
  EXPECT_EQ(DecodeFirst(Bytes({0xF0, 0x90, 0x8D, 0x88})).codepoint, 0x10348u);
}

TEST(CursorTryDecodeUTF8Test, ValidMaxCodePoint) {
  // U+10FFFF  -> 0xF4 0x8F 0xBF 0xBF
  EXPECT_EQ(DecodeFirst(Bytes({0xF4, 0x8F, 0xBF, 0xBF})).codepoint, 0x10FFFFu);
}

//=== Truncated multibyte sequences ===//
//
// Clang treats an incomplete UTF-8 sequence at end-of-input as malformed
// input, not as EOF for the partially decoded character.

TEST(CursorTryDecodeUTF8Test, TruncatedFourByteAfterTwoContinuations) {
  // 0xF0 0x90 0x8D would start a 4-byte sequence, but the final continuation
  // byte is missing.
  EXPECT_TRUE(DecodeFirst(Bytes({0xF0, 0x90, 0x8D})).IsInvalidUTF8());
}

//=== Overlong 2-byte sequences ===//
//
// Clang rejects 0xC0/0xC1 lead bytes: they would encode U+0000..U+007F using
// two bytes, but those code points require only one byte (C11 Annex D / RFC
// 3629 §3).  The current implementation accepts them and returns a valid
// codepoint instead of kInvalid.

TEST(CursorTryDecodeUTF8Test, OverlongTwoByte_C0_80) {
  // 0xC0 0x80  -> overlong encoding of U+0000
  EXPECT_TRUE(DecodeFirst(Bytes({0xC0, 0x80})).IsInvalidUTF8());
}

TEST(CursorTryDecodeUTF8Test, OverlongTwoByte_C1_BF) {
  // 0xC1 0xBF  -> overlong encoding of U+007F
  EXPECT_TRUE(DecodeFirst(Bytes({0xC1, 0xBF})).IsInvalidUTF8());
}

//=== Overlong 3-byte sequences ===//
//
// A 0xE0 lead byte followed by a second byte in 0x80..0x9F encodes a code
// point below U+0800, which requires at most two bytes.  Clang rejects the
// sequence; the current implementation returns a valid codepoint.

TEST(CursorTryDecodeUTF8Test, OverlongThreeByte_E0_9F_BF) {
  // 0xE0 0x9F 0xBF  -> overlong encoding of U+07FF
  EXPECT_TRUE(DecodeFirst(Bytes({0xE0, 0x9F, 0xBF})).IsInvalidUTF8());
}

TEST(CursorTryDecodeUTF8Test, OverlongThreeByte_E0_80_80) {
  // 0xE0 0x80 0x80  -> overlong encoding of U+0000
  EXPECT_TRUE(DecodeFirst(Bytes({0xE0, 0x80, 0x80})).IsInvalidUTF8());
}

//=== Overlong 4-byte sequences ===//
//
// A 0xF0 lead byte followed by a second byte in 0x80..0x8F encodes a code
// point below U+10000, which requires at most three bytes.  Clang rejects
// the sequence; the current implementation returns a valid codepoint.

TEST(CursorTryDecodeUTF8Test, OverlongFourByte_F0_8F_BF_BF) {
  // 0xF0 0x8F 0xBF 0xBF  -> overlong encoding of U+FFFF
  EXPECT_TRUE(DecodeFirst(Bytes({0xF0, 0x8F, 0xBF, 0xBF})).IsInvalidUTF8());
}

TEST(CursorTryDecodeUTF8Test, OverlongFourByte_F0_80_80_80) {
  // 0xF0 0x80 0x80 0x80  -> overlong encoding of U+0000
  EXPECT_TRUE(DecodeFirst(Bytes({0xF0, 0x80, 0x80, 0x80})).IsInvalidUTF8());
}

//=== Surrogate code points ===//
//
// U+D800..U+DFFF are reserved for UTF-16 surrogate pairs and are not valid
// Unicode scalar values.  RFC 3629 §3 explicitly forbids encoding them in
// UTF-8.  Clang rejects them; the current implementation decodes and returns
// them as ordinary codepoints.

TEST(CursorTryDecodeUTF8Test, SurrogateFirst_ED_A0_80) {
  // 0xED 0xA0 0x80  -> U+D800 (first high surrogate)
  EXPECT_TRUE(DecodeFirst(Bytes({0xED, 0xA0, 0x80})).IsInvalidUTF8());
}

TEST(CursorTryDecodeUTF8Test, SurrogateLast_ED_BF_BF) {
  // 0xED 0xBF 0xBF  -> U+DFFF (last low surrogate)
  EXPECT_TRUE(DecodeFirst(Bytes({0xED, 0xBF, 0xBF})).IsInvalidUTF8());
}

//=== Code points above U+10FFFF ===//
//
// The Unicode standard defines U+10FFFF as the maximum code point.  RFC 3629
// §3 restricts UTF-8 to sequences that encode values up to U+10FFFF.  Clang
// rejects sequences that would decode to larger values; the current
// implementation returns them as valid codepoints.

TEST(CursorTryDecodeUTF8Test, OutOfRange_F4_90_80_80) {
  // 0xF4 0x90 0x80 0x80  -> U+110000, one past the Unicode maximum
  EXPECT_TRUE(DecodeFirst(Bytes({0xF4, 0x90, 0x80, 0x80})).IsInvalidUTF8());
}

TEST(CursorTryDecodeUTF8Test, OutOfRange_F5_lead) {
  // 0xF5 lead byte always encodes a value > U+10FFFF
  EXPECT_TRUE(DecodeFirst(Bytes({0xF5, 0x80, 0x80, 0x80})).IsInvalidUTF8());
}

}  // namespace
}  // namespace bcc
