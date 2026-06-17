#include "bcc/lex/lexer.hh"

#include <string_view>

#include "bcc/basic/file_manager.hh"
#include "bcc/basic/source_manager.hh"
#include "gtest/gtest.h"

namespace bcc {
namespace {

struct ExpectedToken {
  TokenKind kind;
  std::string_view lexeme;
  uint32_t offset;
  bool start_of_line;
  bool leading_space;
  bool needs_cleaning;
};

std::string MakeBytes(std::initializer_list<char> bytes) {
  return {bytes.begin(), bytes.end()};
}

class BufferedLexerTest : public ::testing::Test {
 protected:
  FileManager fm_;
  SourceManager sm_{fm_};
  uint32_t base_ = 0;

  BufferedLexer Lex(std::string_view input) {
    FileID fid = sm_.CreateFileID("", std::string(input));
    base_ = sm_.GetLocForStartOfFile(fid).GetOffset();
    return BufferedLexer(sm_, fid);
  }

  void ExpectToken(const Token& token, const ExpectedToken& expected) {
    EXPECT_EQ(token.GetKind(), expected.kind);
    EXPECT_EQ(token.GetLexeme(), expected.lexeme);
    EXPECT_EQ(token.GetLocation().GetOffset() - base_, expected.offset);
    EXPECT_EQ(token.IsStartOfLine(), expected.start_of_line);
    EXPECT_EQ(token.HasLeadingSpace(), expected.leading_space);
    EXPECT_EQ(token.NeedsCleaning(), expected.needs_cleaning);
  }
};

//=== Line splice tests ===//

TEST_F(BufferedLexerTest, BackslashNewlineAtEndOfInput) {
  auto lexer = Lex("\\\n");
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 2, true, false, false});
}

TEST_F(BufferedLexerTest, BackslashAtEndOfInput) {
  auto lexer = Lex("\\");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kUnknown, "\\", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 1, false, false, false});
}

TEST_F(BufferedLexerTest, LineSpliceBeforeFirstToken) {
  const std::string input = MakeBytes({'\\', '\n', 'i', 'n', 't'});
  auto lexer = Lex(input);
  ExpectToken(lexer.NextToken(),
              {TokenKind::kIdentifier, "\\\nint", 0, true, false, true});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 5, false, false, false});
}

TEST_F(BufferedLexerTest, ConsecutiveLineSplicesBeforeToken) {
  const std::string input = MakeBytes({'\\', '\n', '\\', '\n', 'i', 'n', 't'});
  auto lexer = Lex(input);
  ExpectToken(lexer.NextToken(),
              {TokenKind::kIdentifier, "\\\n\\\nint", 0, true, false, true});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 7, false, false, false});
}

TEST_F(BufferedLexerTest, LineSpliceBetweenPunctuatorChars) {
  const std::string input = MakeBytes({'+', '\\', '\n', '='});
  auto lexer = Lex(input);
  ExpectToken(lexer.NextToken(),
              {TokenKind::kPlusEqual, input, 0, true, false, true});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 4, false, false, false});
}

TEST_F(BufferedLexerTest, LineSpliceInsidePunctuatorUsesLongestMatch) {
  const std::string input = MakeBytes({'<', '\\', '\n', '<', '='});
  auto lexer = Lex(input);
  ExpectToken(lexer.NextToken(),
              {TokenKind::kLessLessEqual, input, 0, true, false, true});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 5, false, false, false});
}

TEST_F(BufferedLexerTest, LineSpliceBetweenCommentSlashes) {
  const std::string input = MakeBytes({'/', '\\', '\n', '/', 'x'});
  const std::string comment = MakeBytes({'/', '\\', '\n', '/', 'x'});
  auto lexer = Lex(input);
  ExpectToken(lexer.NextToken(),
              {TokenKind::kComment, comment, 0, true, false, true});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 5, true, true, false});
}

TEST_F(BufferedLexerTest, LineSpliceBetweenSlashAndStarStartsBlockComment) {
  const std::string input = MakeBytes({'/', '\\', '\n', '*', 'x', '*', '/'});
  const std::string comment = input;
  auto lexer = Lex(input);
  ExpectToken(lexer.NextToken(),
              {TokenKind::kComment, comment, 0, true, false, true});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 7, true, true, false});
}

TEST_F(BufferedLexerTest, SingleBackslashBetweenTokens) {
  auto lexer = Lex("\\\\");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kUnknown, "\\", 0, true, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kUnknown, "\\", 1, false, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 2, false, false, false});
}

TEST_F(BufferedLexerTest, BackslashVariantsAtEndOfInput) {
  // Line comment ending with backslash-newline at EOF.
  {
    const std::string input =
        MakeBytes({' ', ' ', '/', '/', ' ', ' ', '\\', '\n'});
    const std::string comment = MakeBytes({'/', '/', ' ', ' ', '\\', '\n'});
    auto lexer = Lex(input);
    ExpectToken(lexer.NextToken(),
                {TokenKind::kWhitespace, "  ", 0, true, false, false});
    ExpectToken(lexer.NextToken(),
                {TokenKind::kComment, comment, 2, true, true, true});
    ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 8, true, true, false});
  }

  // Double-backslash at end of input.
  {
    auto lexer = Lex("#include <\\\\");
    ExpectToken(lexer.NextToken(),
                {TokenKind::kHash, "#", 0, true, false, false});
    ExpectToken(lexer.NextToken(),
                {TokenKind::kIdentifier, "include", 1, false, false, false});
    ExpectToken(lexer.NextToken(),
                {TokenKind::kWhitespace, " ", 8, false, false, false});
    ExpectToken(lexer.NextToken(),
                {TokenKind::kLess, "<", 9, false, true, false});
    ExpectToken(lexer.NextToken(),
                {TokenKind::kUnknown, "\\", 10, false, false, false});
    ExpectToken(lexer.NextToken(),
                {TokenKind::kUnknown, "\\", 11, false, false, false});
    ExpectToken(lexer.NextToken(),
                {TokenKind::kEOF, "", 12, false, false, false});
  }

  // Double-backslash followed by newline at end of input.
  {
    const std::string input = MakeBytes(
        {'#', 'i', 'n', 'c', 'l', 'u', 'd', 'e', ' ', '<', '\\', '\\', '\n'});
    auto lexer = Lex(input);
    ExpectToken(lexer.NextToken(),
                {TokenKind::kHash, "#", 0, true, false, false});
    ExpectToken(lexer.NextToken(),
                {TokenKind::kIdentifier, "include", 1, false, false, false});
    ExpectToken(lexer.NextToken(),
                {TokenKind::kWhitespace, " ", 8, false, false, false});
    ExpectToken(lexer.NextToken(),
                {TokenKind::kLess, "<", 9, false, true, false});
    ExpectToken(lexer.NextToken(),
                {TokenKind::kUnknown, "\\", 10, false, false, false});
    ExpectToken(lexer.NextToken(),
                {TokenKind::kEOF, "", 13, false, false, false});
  }
}

TEST_F(BufferedLexerTest, CharLiteralBackslashAtEndOfInput) {
  auto lexer = Lex("'a\\");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kUnknown, "'a\\", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 3, false, false, false});
}

TEST_F(BufferedLexerTest, CharLiteralEscapeThenLineSplice) {
  // Input bytes:  '  \  \  \n  '
  //
  // After C phase 2 the \<newline> at bytes 2-3 is deleted, leaving '\''.
  // This is a valid char constant per the standard, but Clang and GCC both
  // treat \\ at bytes 1-2 as an escape (consuming both backslashes), then
  // hit the raw newline at byte 3 and produce TokenKind::kUnknown.  We
  // match Clang's behaviour here, which is the de facto reference.
  const std::string input = MakeBytes({'\'', '\\', '\\', '\n', '\''});
  auto lexer = Lex(input);

  ExpectToken(lexer.NextToken(),
              {TokenKind::kUnknown, input, 0, true, false, true});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 5, false, false, false});
}

//=== PP-number tests ===//

TEST_F(BufferedLexerTest, PpNumberHexFloat) {
  auto lexer = Lex("0x1.2p3");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kNumericConstant, "0x1.2p3", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 7, false, false, false});
}

TEST_F(BufferedLexerTest, PpNumberMultipleDots) {
  auto lexer = Lex("1.2.3");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kNumericConstant, "1.2.3", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 5, false, false, false});
}

TEST_F(BufferedLexerTest, PpNumberEndingWithExponentSign) {
  auto lexer = Lex("1e+");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kNumericConstant, "1e+", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 3, false, false, false});
}

TEST_F(BufferedLexerTest, PpNumberExponentWithoutSign) {
  auto lexer = Lex("1e5 1E5");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kNumericConstant, "1e5", 0, true, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kWhitespace, " ", 3, false, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kNumericConstant, "1E5", 4, false, true, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 7, false, false, false});
}

TEST_F(BufferedLexerTest, PpNumberExponentContinueWithDot) {
  auto lexer = Lex("1e0.5");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kNumericConstant, "1e0.5", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 5, false, false, false});
}

TEST_F(BufferedLexerTest, PpNumberExponentWithLetterSuffix) {
  auto lexer = Lex("1e5x");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kNumericConstant, "1e5x", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 4, false, false, false});
}

TEST_F(BufferedLexerTest, PpNumberExponentSignWithUnderscore) {
  auto lexer = Lex("1e+_5");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kNumericConstant, "1e+_5", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 5, false, false, false});
}

TEST_F(BufferedLexerTest, PpNumberUnderscore) {
  auto lexer = Lex("1_2");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kNumericConstant, "1_2", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 3, false, false, false});
}

TEST_F(BufferedLexerTest, PpNumberExponentWithHexDigits) {
  auto lexer = Lex("1e-0x");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kNumericConstant, "1e-0x", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 5, false, false, false});
}

TEST_F(BufferedLexerTest, PpNumberWithIdentifierSuffix) {
  auto lexer = Lex("42foo");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kNumericConstant, "42foo", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 5, false, false, false});
}

TEST_F(BufferedLexerTest, PpNumbersWithExponentSigns) {
  auto lexer = Lex("0x1.fp+2 1.0e-3 .5e+1");

  constexpr ExpectedToken kExpected[] = {
      {TokenKind::kNumericConstant, "0x1.fp+2", 0, true, false, false},
      {TokenKind::kWhitespace, " ", 8, false, false, false},
      {TokenKind::kNumericConstant, "1.0e-3", 9, false, true, false},
      {TokenKind::kWhitespace, " ", 15, false, false, false},
      {TokenKind::kNumericConstant, ".5e+1", 16, false, true, false},
      {TokenKind::kEOF, "", 21, false, false, false},
  };

  for (const auto& expected : kExpected) {
    ExpectToken(lexer.NextToken(), expected);
  }
}

TEST_F(BufferedLexerTest, PpNumberDotLineSpliceDigit) {
  const std::string input = MakeBytes({'.', '\\', '\n', '5'});
  auto lexer = Lex(input);
  ExpectToken(lexer.NextToken(),
              {TokenKind::kNumericConstant, input, 0, true, false, true});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 4, false, false, false});
}

TEST_F(BufferedLexerTest, PpNumberExponentLineSpliceSign) {
  const std::string input = MakeBytes({'1', 'e', '\\', '\n', '+', '5'});
  auto lexer = Lex(input);
  ExpectToken(lexer.NextToken(),
              {TokenKind::kNumericConstant, input, 0, true, false, true});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 6, false, false, false});
}

TEST_F(BufferedLexerTest, PpNumberExponentLineSpliceMinusSign) {
  const std::string input = MakeBytes({'1', 'e', '\\', '\n', '-', '5'});
  auto lexer = Lex(input);
  ExpectToken(lexer.NextToken(),
              {TokenKind::kNumericConstant, input, 0, true, false, true});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 6, false, false, false});
}

TEST_F(BufferedLexerTest, PpNumberLineSpliceBetweenEachChar) {
  const std::string input =
      MakeBytes({'4', '\\', '\n', '2', '\\', '\n', '.', '0'});
  auto lexer = Lex(input);
  ExpectToken(lexer.NextToken(),
              {TokenKind::kNumericConstant, input, 0, true, false, true});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 8, false, false, false});
}

//=== Identifier tests ===//

TEST_F(BufferedLexerTest, Utf8IdentifierAsSingleToken) {
  auto lexer = Lex("变量 = 0");

  ExpectToken(lexer.NextToken(),
              {TokenKind::kIdentifier, "变量", 0, true, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kWhitespace, " ", 6, false, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kEqual, "=", 7, false, true, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kWhitespace, " ", 8, false, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kNumericConstant, "0", 9, false, true, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kEOF, "", 10, false, false, false});
}

TEST_F(BufferedLexerTest, UcnIdentifierDeclaration) {
  auto lexer = Lex("int \\u53d8\\u91cf = 0;");

  ExpectToken(lexer.NextToken(),
              {TokenKind::kIdentifier, "int", 0, true, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kWhitespace, " ", 3, false, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kIdentifier, "\\u53d8\\u91cf", 4,
                                  false, true, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kWhitespace, " ", 16, false, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kEqual, "=", 17, false, true, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kWhitespace, " ", 18, false, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kNumericConstant, "0", 19, false, true, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kSemi, ";", 20, false, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kEOF, "", 21, false, false, false});
}

TEST_F(BufferedLexerTest, PartialUcnAtTokenStart) {
  auto lexer = Lex("\\uQQQQ");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kUnknown, "\\", 0, true, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kIdentifier, "uQQQQ", 1, false, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 6, false, false, false});
}

TEST_F(BufferedLexerTest, UcnIdentifierStart) {
  auto lexer = Lex("\\u00e9");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kIdentifier, "\\u00e9", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 6, false, false, false});
}

TEST_F(BufferedLexerTest, IdentifierContinueWithUcn) {
  auto lexer = Lex("a\\u0030b");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kIdentifier, "a", 0, true, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kUnknown, "\\u0030", 1, false, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kIdentifier, "b", 7, false, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 8, false, false, false});
}

TEST_F(BufferedLexerTest, UcnForBasicCharRejected) {
  // U+0041 is 'A', a character in the basic source character set.
  auto lexer = Lex("\\u0041");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kUnknown, "\\u0041", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 6, false, false, false});
}

//=== Comment tests ===//

TEST_F(BufferedLexerTest, LineCommentBackslashAtEndOfInput) {
  auto lexer = Lex("// \\");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kComment, "// \\", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 4, true, true, false});
}

TEST_F(BufferedLexerTest, UnterminatedBlockComment) {
  auto lexer = Lex("/* unterminated");

  constexpr ExpectedToken kExpected[] = {
      {TokenKind::kUnknown, "/* unterminated", 0, true, false, false},
      {TokenKind::kEOF, "", 15, false, false, false},
  };

  for (const auto& expected : kExpected) {
    ExpectToken(lexer.NextToken(), expected);
  }
}

TEST_F(BufferedLexerTest, NestedBlockComment) {
  auto lexer = Lex("/* /* */");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kComment, "/* /* */", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 8, true, true, false});
}

TEST_F(BufferedLexerTest, BlockCommentStarSlashSeparatedByLineSplice) {
  // Input bytes:  /  *  SPACE  *  \  \n  /
  const std::string input = MakeBytes({'/', '*', ' ', '*', '\\', '\n', '/'});
  auto lexer = Lex(input);

  Token comment = lexer.NextToken();
  ExpectToken(comment, {TokenKind::kComment, input, 0, true, false, true});

  Token eof = lexer.NextToken();
  EXPECT_EQ(eof.GetKind(), TokenKind::kEOF);
  EXPECT_EQ(eof.GetLexeme(), "");
  EXPECT_EQ(eof.GetLocation().GetOffset() - base_, 7u);
  EXPECT_TRUE(eof.IsStartOfLine());
  EXPECT_TRUE(eof.HasLeadingSpace());
  EXPECT_FALSE(eof.NeedsCleaning());
}

//=== String and character literal tests ===//

TEST_F(BufferedLexerTest, EmptyStringLiteral) {
  auto lexer = Lex("\"\"");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kStringLiteral, "\"\"", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 2, false, false, false});
}

TEST_F(BufferedLexerTest, Utf8StringLiteral) {
  auto lexer = Lex("u8\"abc\"");
  ExpectToken(lexer.NextToken(), {TokenKind::kUtf8StringLiteral, "u8\"abc\"", 0,
                                  true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 7, false, false, false});
}

TEST_F(BufferedLexerTest, StringLiteralBackslashAtEndOfInput) {
  auto lexer = Lex("\"foo\\");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kUnknown, "\"foo\\", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 5, false, false, false});
}

TEST_F(BufferedLexerTest, StringLiteralInvalidUtf8) {
  const std::string input = MakeBytes({'"', static_cast<char>(0x80), '"'});
  auto lexer = Lex(input);

  ExpectToken(lexer.NextToken(),
              {TokenKind::kStringLiteral, input, 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 3, false, false, false});
}

TEST_F(BufferedLexerTest, TruncatedFourByteUtf8ProducesBytewiseUnknownTokens) {
  const std::string input =
      MakeBytes({static_cast<char>(0xF0), static_cast<char>(0x90),
                 static_cast<char>(0x8D)});
  auto lexer = Lex(input);

  ExpectToken(lexer.NextToken(),
              {TokenKind::kUnknown, input.substr(0, 1), 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kUnknown, input.substr(1, 1), 1,
                                  false, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kUnknown, input.substr(2, 1), 2,
                                  false, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 3, false, false, false});
}

TEST_F(BufferedLexerTest,
       OverlongTwoByteUtf8AcrossLineSpliceProducesBytewiseUnknownTokens) {
  const std::string input =
      MakeBytes({static_cast<char>(0xC0), '\\', '\n', static_cast<char>(0x80)});
  auto lexer = Lex(input);

  ExpectToken(lexer.NextToken(),
              {TokenKind::kUnknown, "\xc0", 0, true, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kUnknown, "\\\n\x80", 1, false, false, true});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 4, false, false, false});
}

TEST_F(BufferedLexerTest, EmptyCharLiteral) {
  auto lexer = Lex("''");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kCharConstant, "''", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 2, false, false, false});
}

TEST_F(BufferedLexerTest, EmptyWideCharLiteral) {
  auto lexer = Lex("L''");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kWideCharConstant, "L''", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 3, false, false, false});
}

TEST_F(BufferedLexerTest, EmptyUtf16CharLiteral) {
  auto lexer = Lex("u''");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kUtf16CharConstant, "u''", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 3, false, false, false});
}

TEST_F(BufferedLexerTest, EmptyUtf32CharLiteral) {
  auto lexer = Lex("U''");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kUtf32CharConstant, "U''", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 3, false, false, false});
}

TEST_F(BufferedLexerTest, U8CharLiteralIsIdentifierAndCharConstant) {
  auto lexer = Lex("u8'x'");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kIdentifier, "u8", 0, true, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kCharConstant, "'x'", 2, false, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 5, false, false, false});
}

TEST_F(BufferedLexerTest, WideCharLiteralWithContent) {
  auto lexer = Lex("L'x'");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kWideCharConstant, "L'x'", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 4, false, false, false});
}

TEST_F(BufferedLexerTest, UnterminatedCharLiteral) {
  auto lexer = Lex("'a");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kUnknown, "'a", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 2, false, false, false});
}

TEST_F(BufferedLexerTest, UnterminatedStringLiteral) {
  auto lexer = Lex("\"hello");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kUnknown, "\"hello", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 6, false, false, false});
}

TEST_F(BufferedLexerTest, NewlineInsideStringLiteral) {
  auto lexer = Lex("\"a\nb\"");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kUnknown, "\"a", 0, true, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kNewLine, "\n", 2, false, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kIdentifier, "b", 3, true, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kUnknown, "\"", 4, false, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 5, false, false, false});
}

TEST_F(BufferedLexerTest, NewlineInsideCharLiteral) {
  auto lexer = Lex("'a\nb'");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kUnknown, "'a", 0, true, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kNewLine, "\n", 2, false, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kIdentifier, "b", 3, true, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kUnknown, "'", 4, false, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 5, false, false, false});
}

//=== Punctuator tests ===//

TEST_F(BufferedLexerTest, LongestPunctuatorMatchWithUnknownFallback) {
  auto lexer = Lex(">>=>>...##@");

  constexpr ExpectedToken kExpected[] = {
      {TokenKind::kGreaterGreaterEqual, ">>=", 0, true, false, false},
      {TokenKind::kGreaterGreater, ">>", 3, false, false, false},
      {TokenKind::kEllipsis, "...", 5, false, false, false},
      {TokenKind::kHashHash, "##", 8, false, false, false},
      {TokenKind::kUnknown, "@", 10, false, false, false},
      {TokenKind::kEOF, "", 11, false, false, false},
  };

  for (const auto& expected : kExpected) {
    ExpectToken(lexer.NextToken(), expected);
  }
}

TEST_F(BufferedLexerTest, SlashEqualToken) {
  auto lexer = Lex("/=");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kSlashEqual, "/=", 0, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 2, false, false, false});
}

TEST_F(BufferedLexerTest, IdentifierStartingWithEncodingPrefix) {
  auto lexer = Lex("uint union Local");
  ExpectToken(lexer.NextToken(),
              {TokenKind::kIdentifier, "uint", 0, true, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kWhitespace, " ", 4, false, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kIdentifier, "union", 5, false, true, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kWhitespace, " ", 10, false, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kIdentifier, "Local", 11, false, true, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kEOF, "", 16, false, false, false});
}

//=== Whitespace and newline tests ===//

TEST_F(BufferedLexerTest, CarriageReturnAsNewline) {
  auto lexer = Lex("a\rb\r\nc");

  ExpectToken(lexer.NextToken(),
              {TokenKind::kIdentifier, "a", 0, true, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kNewLine, "\r", 1, false, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kIdentifier, "b", 2, true, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kNewLine, "\r\n", 3, false, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kIdentifier, "c", 5, true, false, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 6, false, false, false});
}

TEST_F(BufferedLexerTest, LeadingSpaceAcrossCommentsAndNewlines) {
  auto lexer = Lex(" \t/*gap*/name\n// tail\nnext");

  constexpr ExpectedToken kExpected[] = {
      {TokenKind::kWhitespace, " \t", 0, true, false, false},
      {TokenKind::kComment, "/*gap*/", 2, true, true, false},
      {TokenKind::kIdentifier, "name", 9, true, true, false},
      {TokenKind::kNewLine, "\n", 13, false, false, false},
      {TokenKind::kComment, "// tail", 14, true, false, false},
      {TokenKind::kNewLine, "\n", 21, true, true, false},
      {TokenKind::kIdentifier, "next", 22, true, false, false},
      {TokenKind::kEOF, "", 26, false, false, false},
  };

  for (const auto& expected : kExpected) {
    ExpectToken(lexer.NextToken(), expected);
  }
}

//=== Mixed token stream tests ===//

TEST_F(BufferedLexerTest, MixedTokenStream) {
  auto lexer = Lex("foo /*x*/bar\n baz+=1.0e-3");

  constexpr ExpectedToken kExpected[] = {
      {TokenKind::kIdentifier, "foo", 0, true, false, false},
      {TokenKind::kWhitespace, " ", 3, false, false, false},
      {TokenKind::kComment, "/*x*/", 4, false, true, false},
      {TokenKind::kIdentifier, "bar", 9, false, true, false},
      {TokenKind::kNewLine, "\n", 12, false, false, false},
      {TokenKind::kWhitespace, " ", 13, true, false, false},
      {TokenKind::kIdentifier, "baz", 14, true, true, false},
      {TokenKind::kPlusEqual, "+=", 17, false, false, false},
      {TokenKind::kNumericConstant, "1.0e-3", 19, false, false, false},
      {TokenKind::kEOF, "", 25, false, false, false},
  };

  for (const auto& expected : kExpected) {
    ExpectToken(lexer.NextToken(), expected);
  }
}

TEST_F(BufferedLexerTest, LineSplicedTokensMarkedForCleaning) {
  const std::string input =
      MakeBytes({'a', 'b', '\\', '\n', 'c', 'd', ' ', '"', 'x', '\\', '\n',
                 'y', '"', ' ', '1', '\\', '\n', '2'});
  const std::string identifier = MakeBytes({'a', 'b', '\\', '\n', 'c', 'd'});
  const std::string string_literal =
      MakeBytes({'"', 'x', '\\', '\n', 'y', '"'});
  const std::string numeric = MakeBytes({'1', '\\', '\n', '2'});
  auto lexer = Lex(input);

  const ExpectedToken kExpected[] = {
      {TokenKind::kIdentifier, identifier, 0, true, false, true},
      {TokenKind::kWhitespace, " ", 6, false, false, false},
      {TokenKind::kStringLiteral, string_literal, 7, false, true, true},
      {TokenKind::kWhitespace, " ", 13, false, false, false},
      {TokenKind::kNumericConstant, numeric, 14, false, true, true},
      {TokenKind::kEOF, "", 18, false, false, false},
  };

  for (const auto& expected : kExpected) {
    ExpectToken(lexer.NextToken(), expected);
  }
}

//=== Edge case tests ===//

TEST_F(BufferedLexerTest, NullBytesBetweenTokens) {
  const std::string input =
      MakeBytes({'i', 'n', 't', '\0', 'x', '\0', '=', '\0', '1'});
  auto lexer = Lex(input);

  ExpectToken(lexer.NextToken(),
              {TokenKind::kIdentifier, "int", 0, true, false, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kIdentifier, "x", 4, false, true, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kEqual, "=", 6, false, true, false});
  ExpectToken(lexer.NextToken(),
              {TokenKind::kNumericConstant, "1", 8, false, true, false});
  ExpectToken(lexer.NextToken(), {TokenKind::kEOF, "", 9, false, false, false});
}

// Null bytes should set LeadingSpace (matching Clang behavior where null
// bytes are treated as whitespace). Currently our lexer skips null bytes
// without updating has_leading_space_, so this test documents the gap.
TEST_F(BufferedLexerTest, NullBytesSetLeadingSpace) {
  // Leading null byte before an identifier: Clang sets LeadingSpace.
  {
    auto lexer = Lex(MakeBytes({'\0', 'i', 'n', 't'}));
    ExpectToken(lexer.NextToken(),
                {TokenKind::kIdentifier, "int", 1, true, true, false});
    ExpectToken(lexer.NextToken(),
                {TokenKind::kEOF, "", 4, false, false, false});
  }

  // Null byte between two identifiers: Clang sets LeadingSpace on the second.
  {
    auto lexer = Lex(MakeBytes({'i', 'n', 't', '\0', 'x'}));
    ExpectToken(lexer.NextToken(),
                {TokenKind::kIdentifier, "int", 0, true, false, false});
    ExpectToken(lexer.NextToken(),
                {TokenKind::kIdentifier, "x", 4, false, true, false});
    ExpectToken(lexer.NextToken(),
                {TokenKind::kEOF, "", 5, false, false, false});
  }

  // Multiple consecutive null bytes: Clang treats them as a single
  // whitespace run (still LeadingSpace).
  {
    auto lexer = Lex(MakeBytes({'i', 'n', 't', '\0', '\0', 'x'}));
    ExpectToken(lexer.NextToken(),
                {TokenKind::kIdentifier, "int", 0, true, false, false});
    ExpectToken(lexer.NextToken(),
                {TokenKind::kIdentifier, "x", 5, false, true, false});
    ExpectToken(lexer.NextToken(),
                {TokenKind::kEOF, "", 6, false, false, false});
  }
}

}  // namespace
}  // namespace bcc
