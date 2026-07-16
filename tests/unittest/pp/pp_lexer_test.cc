#include "bcc/pp/pp_lexer.hh"

#include <string_view>

#include "bcc/basic/file_manager.hh"
#include "bcc/basic/source_manager.hh"
#include "bcc/lex/token_kind.hh"
#include "gtest/gtest.h"

namespace bcc {
namespace {

class PPLexerTest : public ::testing::Test {
 protected:
  FileManager fm_;
  SourceManager sm_{fm_};

  PPLexer Make(std::string_view input) {
    FileID fid = sm_.CreateFileID("", std::string(input));
    return PPLexer(sm_, fid);
  }
};

TEST_F(PPLexerTest, DropsWhitespaceCommentsAndNewlinesOutsideDirectives) {
  auto pp = Make("int  x;\n  // c\n  y /*d*/ z");

  struct Expected {
    TokenKind kind;
    std::string_view lexeme;
    bool start_of_line;
    bool leading_space;
  };
  const Expected expected[] = {
      {TokenKind::kIdentifier, "int", true, false},
      {TokenKind::kIdentifier, "x", false, true},
      {TokenKind::kSemi, ";", false, false},
      {TokenKind::kIdentifier, "y", true, true},
      {TokenKind::kIdentifier, "z", false, true},
      {TokenKind::kEOF, "", false, false},
  };

  for (const auto& e : expected) {
    Token t = pp.Lex();
    EXPECT_EQ(t.GetKind(), e.kind) << e.lexeme;
    EXPECT_EQ(t.GetLexeme(), e.lexeme);
    EXPECT_EQ(t.IsStartOfLine(), e.start_of_line) << e.lexeme;
    EXPECT_EQ(t.HasLeadingSpace(), e.leading_space) << e.lexeme;
  }
}

TEST_F(PPLexerTest, EmitsEodAtNewlineWhileParsingDirective) {
  auto pp = Make("#define X 1\nafter");

  Token hash = pp.Lex();
  EXPECT_EQ(hash.GetKind(), TokenKind::kHash);
  EXPECT_TRUE(hash.IsStartOfLine());

  // The preprocessor would set this after seeing the leading '#'.
  pp.SetParsingPreprocessorDirective(true);

  EXPECT_EQ(pp.Lex().GetLexeme(), "define");
  EXPECT_EQ(pp.Lex().GetLexeme(), "X");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");

  Token eod = pp.Lex();
  EXPECT_EQ(eod.GetKind(), TokenKind::kEod);
  EXPECT_EQ(eod.GetLexeme().size(), 0u);
  // Producing kEod clears the directive flag.
  EXPECT_FALSE(pp.IsParsingPreprocessorDirective());

  Token after = pp.Lex();
  EXPECT_EQ(after.GetKind(), TokenKind::kIdentifier);
  EXPECT_EQ(after.GetLexeme(), "after");
  EXPECT_TRUE(after.IsStartOfLine());
}

TEST_F(PPLexerTest, SynthesizesEodAtEndOfFileWithinDirective) {
  auto pp = Make("#undef X");  // no trailing newline

  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kHash);
  pp.SetParsingPreprocessorDirective(true);
  EXPECT_EQ(pp.Lex().GetLexeme(), "undef");
  EXPECT_EQ(pp.Lex().GetLexeme(), "X");

  Token eod = pp.Lex();
  EXPECT_EQ(eod.GetKind(), TokenKind::kEod);
  EXPECT_FALSE(pp.IsParsingPreprocessorDirective());

  // The EOF token follows and is idempotent.
  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kEOF);
  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kEOF);
}

TEST_F(PPLexerTest, NewlineOutsideDirectiveIsNotEod) {
  auto pp = Make("a\nb");
  EXPECT_EQ(pp.Lex().GetLexeme(), "a");
  Token b = pp.Lex();
  EXPECT_EQ(b.GetKind(), TokenKind::kIdentifier);
  EXPECT_EQ(b.GetLexeme(), "b");
  EXPECT_TRUE(b.IsStartOfLine());
}

TEST_F(PPLexerTest, LexesAngleHeaderName) {
  auto pp = Make("#include <sys/types.h>\nnext");

  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kHash);
  pp.SetParsingPreprocessorDirective(true);
  EXPECT_EQ(pp.Lex().GetLexeme(), "include");

  Token name = pp.LexIncludeFilename();
  EXPECT_EQ(name.GetKind(), TokenKind::kHeaderName);
  EXPECT_EQ(name.GetLexeme(), "<sys/types.h>");
  EXPECT_TRUE(name.HasLeadingSpace());

  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kEod);
  EXPECT_EQ(pp.Lex().GetLexeme(), "next");
}

TEST_F(PPLexerTest, LexesQuotedHeaderNameWithLiteralBackslash) {
  // Backslashes are literal inside a header name (no escape processing).
  auto pp = Make("#include \"a\\b.h\"\n");

  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kHash);
  pp.SetParsingPreprocessorDirective(true);
  EXPECT_EQ(pp.Lex().GetLexeme(), "include");

  Token name = pp.LexIncludeFilename();
  EXPECT_EQ(name.GetKind(), TokenKind::kHeaderName);
  EXPECT_EQ(name.GetLexeme(), "\"a\\b.h\"");

  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kEod);
}

TEST_F(PPLexerTest, SkipsCommentsBeforeHeaderName) {
  auto pp = Make("#include /* comment */ <commented.h>\n");

  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kHash);
  pp.SetParsingPreprocessorDirective(true);
  EXPECT_EQ(pp.Lex().GetLexeme(), "include");

  Token name = pp.LexIncludeFilename();
  EXPECT_EQ(name.GetKind(), TokenKind::kHeaderName);
  EXPECT_EQ(name.GetLexeme(), "<commented.h>");
  EXPECT_TRUE(name.HasLeadingSpace());

  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kEod);
}

TEST_F(PPLexerTest, LexesLineSplicedHeaderName) {
  auto pp = Make("#include <foo\\\nbar.h>\n");

  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kHash);
  pp.SetParsingPreprocessorDirective(true);
  EXPECT_EQ(pp.Lex().GetLexeme(), "include");

  Token name = pp.LexIncludeFilename();
  EXPECT_EQ(name.GetKind(), TokenKind::kHeaderName);
  EXPECT_EQ(name.GetLexeme(), "<foo\\\nbar.h>");
  EXPECT_TRUE(name.NeedsCleaning());

  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kEod);
}

TEST_F(PPLexerTest, ComputedIncludeFallsBackToOrdinaryToken) {
  auto pp = Make("#include MACRO\n");

  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kHash);
  pp.SetParsingPreprocessorDirective(true);
  EXPECT_EQ(pp.Lex().GetLexeme(), "include");

  Token tok = pp.LexIncludeFilename();
  EXPECT_EQ(tok.GetKind(), TokenKind::kIdentifier);
  EXPECT_EQ(tok.GetLexeme(), "MACRO");

  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kEod);
}

TEST_F(PPLexerTest, MissingFilenameYieldsEod) {
  auto pp = Make("#include\n");

  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kHash);
  pp.SetParsingPreprocessorDirective(true);
  EXPECT_EQ(pp.Lex().GetLexeme(), "include");

  Token tok = pp.LexIncludeFilename();
  EXPECT_EQ(tok.GetKind(), TokenKind::kEod);
  EXPECT_FALSE(pp.IsParsingPreprocessorDirective());
}

TEST_F(PPLexerTest, UnterminatedAngleNameFallsBackToLess) {
  // No closing '>' before the newline: the '<' is re-lexed as an ordinary
  // token so the caller can diagnose it.
  auto pp = Make("#include <stdio.h\n");

  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kHash);
  pp.SetParsingPreprocessorDirective(true);
  EXPECT_EQ(pp.Lex().GetLexeme(), "include");

  Token tok = pp.LexIncludeFilename();
  EXPECT_EQ(tok.GetKind(), TokenKind::kLess);
  EXPECT_EQ(tok.GetLexeme(), "<");
}

TEST_F(PPLexerTest, ConditionalStackPushPeekPop) {
  auto pp = Make("");
  EXPECT_EQ(pp.GetConditionalStackDepth(), 0u);

  pp.PushConditionalLevel(SourceLocation{}, /*was_skipping=*/false,
                          /*found_non_skip=*/true, /*found_else=*/false);
  EXPECT_EQ(pp.GetConditionalStackDepth(), 1u);
  EXPECT_TRUE(pp.PeekConditionalLevel().found_non_skip);

  PPConditionalInfo info;
  EXPECT_FALSE(pp.PopConditionalLevel(info));
  EXPECT_TRUE(info.found_non_skip);
  EXPECT_EQ(pp.GetConditionalStackDepth(), 0u);

  // Popping an empty stack reports emptiness.
  EXPECT_TRUE(pp.PopConditionalLevel(info));
}

}  // namespace
}  // namespace bcc
