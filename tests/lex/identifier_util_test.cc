#include "bcc/lex/identifier_util.hh"

#include <cstdint>

#include "gtest/gtest.h"

namespace bcc {
namespace {

struct IdentifierExpectation {
  uint32_t code_point;
  bool is_start;
  bool is_continue;
};

TEST(IdentifierUtilTest, AcceptsAsciiIdentifierCharacters) {
  constexpr IdentifierExpectation kCases[] = {
      {'_', true, true},   {'a', true, true},   {'Z', true, true},
      {'$', true, true},   {'0', false, true},  {'9', false, true},
      {'/', false, false}, {'@', false, false},
  };

  for (const auto& test : kCases) {
    EXPECT_EQ(IsIdentifierStart(test.code_point), test.is_start)
        << "code point: " << test.code_point;
    EXPECT_EQ(IsIdentifierContinue(test.code_point), test.is_continue)
        << "code point: " << test.code_point;
  }
}

TEST(IdentifierUtilTest, RespectsSinglePointAndGapBoundaries) {
  constexpr IdentifierExpectation kCases[] = {
      {0x00A7, false, false}, {0x00A8, true, true},   {0x00A9, false, false},
      {0x00AA, true, true},   {0x00AB, false, false}, {0x00AD, true, true},
      {0x00AE, false, false}, {0x00AF, true, true},   {0x00B1, false, false},
      {0x00B2, true, true},   {0x00B5, true, true},   {0x00B6, false, false},
  };

  for (const auto& test : kCases) {
    EXPECT_EQ(IsIdentifierStart(test.code_point), test.is_start)
        << "code point: 0x" << std::hex << test.code_point;
    EXPECT_EQ(IsIdentifierContinue(test.code_point), test.is_continue)
        << "code point: 0x" << std::hex << test.code_point;
  }
}

TEST(IdentifierUtilTest, AllowsCombiningMarksOnlyAsContinuation) {
  constexpr IdentifierExpectation kCases[] = {
      {0x02FF, true, true},  {0x0300, false, true}, {0x036F, false, true},
      {0x0370, true, true},  {0x1DBF, true, true},  {0x1DC0, false, true},
      {0x1DFF, false, true}, {0x1E00, true, true},  {0x20CF, true, true},
      {0x20D0, false, true}, {0x20FF, false, true}, {0x2100, true, true},
      {0xFE1F, true, true},  {0xFE20, false, true}, {0xFE2F, false, true},
      {0xFE30, true, true},
  };

  for (const auto& test : kCases) {
    EXPECT_EQ(IsIdentifierStart(test.code_point), test.is_start)
        << "code point: 0x" << std::hex << test.code_point;
    EXPECT_EQ(IsIdentifierContinue(test.code_point), test.is_continue)
        << "code point: 0x" << std::hex << test.code_point;
  }
}

TEST(IdentifierUtilTest, HandlesLargeUnicodeRangeBoundaries) {
  constexpr IdentifierExpectation kCases[] = {
      {0x0FFFF, false, false}, {0x10000, true, true},
      {0x1FFFD, true, true},   {0x1FFFE, false, false},
      {0x1FFFF, false, false}, {0x20000, true, true},
      {0x2FFFD, true, true},   {0x2FFFE, false, false},
      {0xE0000, true, true},   {0xEFFFD, true, true},
      {0xEFFFE, false, false}, {0x10FFFF, false, false},
  };

  for (const auto& test : kCases) {
    EXPECT_EQ(IsIdentifierStart(test.code_point), test.is_start)
        << "code point: 0x" << std::hex << test.code_point;
    EXPECT_EQ(IsIdentifierContinue(test.code_point), test.is_continue)
        << "code point: 0x" << std::hex << test.code_point;
  }
}

TEST(IdentifierUtilTest, InRangeMatchesRangeBoundaries) {
  constexpr CodePointRange kRanges[] = {
      {10, 20},
      {30, 40},
      {100, 100},
  };

  EXPECT_FALSE(detail::InRange(kRanges, 9));
  EXPECT_TRUE(detail::InRange(kRanges, 10));
  EXPECT_TRUE(detail::InRange(kRanges, 20));
  EXPECT_FALSE(detail::InRange(kRanges, 21));
  EXPECT_FALSE(detail::InRange(kRanges, 29));
  EXPECT_TRUE(detail::InRange(kRanges, 30));
  EXPECT_TRUE(detail::InRange(kRanges, 40));
  EXPECT_FALSE(detail::InRange(kRanges, 41));
  EXPECT_FALSE(detail::InRange(kRanges, 99));
  EXPECT_TRUE(detail::InRange(kRanges, 100));
  EXPECT_FALSE(detail::InRange(kRanges, 101));
}

}  // namespace
}  // namespace bcc
