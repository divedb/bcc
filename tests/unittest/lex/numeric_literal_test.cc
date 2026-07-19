#include "bcc/lex/numeric_literal.hh"

#include "gtest/gtest.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APSInt.h"

namespace bcc {
namespace {

TEST(NumericLiteralParserTest, ParsesIntegerRadicesIntoAPSInt) {
  for (auto [spelling, expected] :
       {std::pair{"42", 42u}, std::pair{"077", 63u}, std::pair{"0xff", 255u},
        std::pair{"0b1010", 10u}}) {
    NumericLiteralParser literal(spelling);
    ASSERT_FALSE(literal.HadError()) << spelling;
    ASSERT_TRUE(literal.IsIntegerLiteral());
    llvm::APSInt value;
    EXPECT_FALSE(literal.GetIntegerValue(value, 64));
    EXPECT_EQ(value.getZExtValue(), expected);
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
  llvm::APSInt value;
  EXPECT_TRUE(literal.GetIntegerValue(value, 64));
}

TEST(NumericLiteralParserTest, ParsesAPFloatValuesAndSuffixSemantics) {
  NumericLiteralParser literal("0x1.8p+1f");
  ASSERT_FALSE(literal.HadError());
  ASSERT_TRUE(literal.IsFloatingLiteral());
  llvm::APFloat value(llvm::APFloat::IEEEsingle());
  auto status = literal.GetFloatValue(value);
  ASSERT_TRUE(static_cast<bool>(status));
  EXPECT_EQ(*status, llvm::APFloat::opOK);
  EXPECT_DOUBLE_EQ(value.convertToDouble(), 3.0);

  NumericLiteralParser leading_period(".5");
  ASSERT_FALSE(leading_period.HadError());
  llvm::APFloat half(llvm::APFloat::IEEEdouble());
  auto half_status = leading_period.GetFloatValue(half);
  ASSERT_TRUE(static_cast<bool>(half_status));
  EXPECT_DOUBLE_EQ(half.convertToDouble(), 0.5);

  NumericLiteralParser hex_leading_period("0x.8p1");
  ASSERT_FALSE(hex_leading_period.HadError());
  llvm::APFloat one(llvm::APFloat::IEEEdouble());
  auto one_status = hex_leading_period.GetFloatValue(one);
  ASSERT_TRUE(static_cast<bool>(one_status));
  EXPECT_DOUBLE_EQ(one.convertToDouble(), 1.0);
}

TEST(NumericLiteralParserTest, RejectsMalformedValues) {
  for (const char* spelling : {"099", "0x", "1e+", "1''2"}) {
    NumericLiteralParser literal(spelling);
    EXPECT_TRUE(literal.HadError()) << spelling;
  }
}

}  // namespace
}  // namespace bcc
