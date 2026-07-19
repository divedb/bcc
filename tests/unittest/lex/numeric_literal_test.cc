#include "bcc/lex/numeric_literal.hh"

#include "gtest/gtest.h"

namespace bcc {
namespace {

TEST(NumericLiteralParserTest, ParsesIntegerRadicesIntoAPSInt) {
  for (auto [spelling, expected] :
       {std::pair{"42", 42u}, std::pair{"077", 63u}, std::pair{"0xff", 255u},
        std::pair{"0b1010", 10u}}) {
    NumericLiteralParser literal(spelling);
    ASSERT_FALSE(literal.HadError()) << spelling;
    ASSERT_TRUE(literal.IsIntegerLiteral());
    APSInt value;
    EXPECT_FALSE(literal.GetIntegerValue(value, 64));
    EXPECT_EQ(value.GetZExtValue(), expected);

    auto ap_value = literal.GetValue(64);
    ASSERT_TRUE(static_cast<bool>(ap_value));
    ASSERT_TRUE(ap_value.Value().IsInt());
    EXPECT_EQ(ap_value.Value().GetInt().GetZExtValue(), expected);
  }
}

TEST(NumericLiteralParserTest, PreservesIntegerSuffixMetadata) {
  NumericLiteralParser literal("123ULL");
  ASSERT_FALSE(literal.HadError());
  EXPECT_TRUE(literal.IsUnsigned());
  EXPECT_TRUE(literal.IsLongLong());
  EXPECT_FALSE(literal.IsLong());
}

TEST(NumericLiteralParserTest, RejectsInvalidIntegerSuffixes) {
  for (const char* spelling : {"4uu", "123lll", "123lul", "1lL"}) {
    NumericLiteralParser literal(spelling);
    EXPECT_EQ(literal.GetError(), NumericLiteralParser::Error::kInvalidSuffix)
        << spelling;
  }
}

TEST(NumericLiteralParserTest, DetectsAPSIntOverflow) {
  NumericLiteralParser literal("0x10000000000000000");
  ASSERT_FALSE(literal.HadError());
  APSInt value;
  EXPECT_TRUE(literal.GetIntegerValue(value, 64));
  auto ap_value = literal.GetValue(64);
  ASSERT_FALSE(static_cast<bool>(ap_value));
  EXPECT_EQ(ap_value.Error(), NumericLiteralParser::Error::kOverflow);
}

TEST(NumericLiteralParserTest, ParsesAPFloatValuesAndSuffixSemantics) {
  NumericLiteralParser literal("0x1.8p+1f");
  ASSERT_FALSE(literal.HadError());
  ASSERT_TRUE(literal.IsFloatingLiteral());
  auto value = literal.GetFloatValue();
  ASSERT_TRUE(static_cast<bool>(value));
  EXPECT_DOUBLE_EQ(value.Value().ToDouble(), 3.0);

  NumericLiteralParser leading_period(".5");
  ASSERT_FALSE(leading_period.HadError());
  auto half = leading_period.GetFloatValue();
  ASSERT_TRUE(static_cast<bool>(half));
  EXPECT_DOUBLE_EQ(half.Value().ToDouble(), 0.5);

  NumericLiteralParser hex_leading_period("0x.8p1");
  ASSERT_FALSE(hex_leading_period.HadError());
  auto one = hex_leading_period.GetFloatValue();
  ASSERT_TRUE(static_cast<bool>(one));
  EXPECT_DOUBLE_EQ(one.Value().ToDouble(), 1.0);

  auto ap_value = literal.GetValue();
  ASSERT_TRUE(static_cast<bool>(ap_value));
  ASSERT_TRUE(ap_value.Value().IsFloat());
  EXPECT_DOUBLE_EQ(ap_value.Value().GetFloat().ToDouble(), 3.0);
}

TEST(NumericLiteralParserTest, RejectsMalformedValues) {
  for (const char* spelling : {"099", "0x", "1e+", "1''2"}) {
    NumericLiteralParser literal(spelling);
    EXPECT_TRUE(literal.HadError()) << spelling;
  }
}

}  // namespace
}  // namespace bcc
