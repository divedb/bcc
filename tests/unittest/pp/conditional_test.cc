#include <string>
#include <string_view>
#include <vector>

#include "bcc/basic/diagnostic.hh"
#include "bcc/basic/file_manager.hh"
#include "bcc/basic/source_manager.hh"
#include "bcc/pp/preprocessor.hh"
#include "bcc/lex/token_kind.hh"
#include "gtest/gtest.h"

namespace bcc {
namespace {

class ConditionalTest : public ::testing::Test {
 protected:
  FileManager fm_;
  SourceManager sm_{fm_};
  DiagnosticsEngine diags_{nullptr, &sm_};

  std::vector<std::string> Run(std::string_view content) {
    sm_.SetMainFileID(sm_.CreateFileID("main.c", std::string(content)));
    Preprocessor pp(sm_, diags_);
    pp.EnterMainFile();

    std::vector<std::string> out;
    for (;;) {
      Token t = pp.Lex();
      if (t.GetKind() == TokenKind::kEOF) break;
      out.push_back(std::string(t.GetLexeme()));
    }
    return out;
  }
};

using V = std::vector<std::string>;

TEST_F(ConditionalTest, IfTrueKeepsBody) {
  EXPECT_EQ(Run("#if 1\nyes\n#endif\n"), (V{"yes"}));
}

TEST_F(ConditionalTest, IfFalseDropsBody) {
  EXPECT_EQ(Run("#if 0\nno\n#endif\nafter"), (V{"after"}));
}

TEST_F(ConditionalTest, IfElseTakesElseWhenFalse) {
  EXPECT_EQ(Run("#if 0\na\n#else\nb\n#endif\n"), (V{"b"}));
}

TEST_F(ConditionalTest, IfElseTakesIfWhenTrue) {
  EXPECT_EQ(Run("#if 1\na\n#else\nb\n#endif\n"), (V{"a"}));
}

TEST_F(ConditionalTest, ArithmeticExpression) {
  EXPECT_EQ(Run("#if 2 + 3 * 4 == 14\nok\n#endif\n"), (V{"ok"}));
}

TEST_F(ConditionalTest, ParenthesesAndComparison) {
  EXPECT_EQ(Run("#if (1 + 1) * 3 > 5\nok\n#endif\n"), (V{"ok"}));
}

TEST_F(ConditionalTest, LogicalAndOr) {
  EXPECT_EQ(Run("#if 1 && 0 || 1\nok\n#endif\n"), (V{"ok"}));
}

TEST_F(ConditionalTest, UnaryAndBitwise) {
  EXPECT_EQ(Run("#if !0 && (~0 & 0xF) == 15\nok\n#endif\n"), (V{"ok"}));
}

TEST_F(ConditionalTest, TernaryOperator) {
  EXPECT_EQ(Run("#if 1 ? 2 : 0\nok\n#endif\n"), (V{"ok"}));
}

TEST_F(ConditionalTest, UnsignedComparison) {
  // -1 as unsigned is the largest value, so it is > 0 in unsigned comparison.
  EXPECT_EQ(Run("#if -1 > 0u\nok\n#endif\n"), (V{"ok"}));
  EXPECT_EQ(Run("#if -1 > 0\nsigned\n#else\nno\n#endif\n"), (V{"no"}));
}

TEST_F(ConditionalTest, UndefinedIdentifierIsZero) {
  EXPECT_EQ(Run("#if UNDEFINED\nno\n#else\nyes\n#endif\n"), (V{"yes"}));
}

TEST_F(ConditionalTest, DefinedOperatorBareAndParen) {
  EXPECT_EQ(Run("#define FOO 1\n#if defined FOO\na\n#endif\n"), (V{"a"}));
  EXPECT_EQ(Run("#define FOO 1\n#if defined(FOO)\nb\n#endif\n"), (V{"b"}));
  EXPECT_EQ(Run("#if defined(BAR)\nno\n#else\nc\n#endif\n"), (V{"c"}));
}

TEST_F(ConditionalTest, DefinedOperandIsNotExpanded) {
  // FOO is defined to 0; `defined FOO` must test definedness, not expand FOO.
  EXPECT_EQ(Run("#define FOO 0\n#if defined FOO\nok\n#endif\n"), (V{"ok"}));
}

TEST_F(ConditionalTest, MacroExpandedInCondition) {
  EXPECT_EQ(Run("#define N 3\n#if N == 3\nok\n#endif\n"), (V{"ok"}));
}

TEST_F(ConditionalTest, FunctionMacroInCondition) {
  EXPECT_EQ(Run("#define ADD(a, b) ((a) + (b))\n#if ADD(2, 3) == 5\nok\n#endif\n"),
            (V{"ok"}));
}

TEST_F(ConditionalTest, IfdefAndIfndef) {
  EXPECT_EQ(Run("#define X\n#ifdef X\na\n#endif\n#ifndef Y\nb\n#endif\n"),
            (V{"a", "b"}));
  EXPECT_EQ(Run("#ifdef Z\nno\n#endif\nafter"), (V{"after"}));
}

TEST_F(ConditionalTest, ElifChainSelectsFirstTrue) {
  EXPECT_EQ(Run("#if 0\na\n#elif 0\nb\n#elif 1\nc\n#elif 1\nd\n#else\ne\n#endif\n"),
            (V{"c"}));
}

TEST_F(ConditionalTest, ElifChainFallsToElse) {
  EXPECT_EQ(Run("#if 0\na\n#elif 0\nb\n#else\nc\n#endif\n"), (V{"c"}));
}

TEST_F(ConditionalTest, ElifNotEvaluatedAfterTrueBranch) {
  // The true #if branch is taken; the #elif condition (1/0) must NOT be
  // evaluated, so no division-by-zero diagnostic is produced.
  EXPECT_EQ(Run("#if 1\na\n#elif 1/0\nb\n#endif\n"), (V{"a"}));
  EXPECT_EQ(diags_.NumErrors(), 0u);
}

TEST_F(ConditionalTest, NestedConditionals) {
  EXPECT_EQ(Run("#if 1\n#if 0\na\n#else\nb\n#endif\n#endif\n"), (V{"b"}));
}

TEST_F(ConditionalTest, SkippedBranchIsNotMacroDefined) {
  // The #define in the dead branch must not take effect.
  EXPECT_EQ(Run("#if 0\n#define M 1\n#endif\nM"), (V{"M"}));
}

TEST_F(ConditionalTest, SkippedBranchIgnoresGarbageConditions) {
  // Conditions inside a skipped block are not evaluated, so `#if 1/0` and a
  // bogus expression cause no diagnostics.
  EXPECT_EQ(Run("#if 0\n#if 1/0\nx\n#endif\n#endif\nok"), (V{"ok"}));
  EXPECT_EQ(diags_.NumErrors(), 0u);
}

TEST_F(ConditionalTest, IncludeGuardPattern) {
  // Second inclusion of the same guarded content is fully skipped.
  const char* content =
      "#ifndef GUARD\n"
      "#define GUARD\n"
      "content\n"
      "#endif\n"
      "#ifndef GUARD\n"
      "again\n"
      "#endif\n";
  EXPECT_EQ(Run(content), (V{"content"}));
}

TEST_F(ConditionalTest, CharConstantInCondition) {
  EXPECT_EQ(Run("#if 'A' == 65\nok\n#endif\n"), (V{"ok"}));
}

TEST_F(ConditionalTest, EndifWithoutIfReportsError) {
  Run("#endif\n");
  EXPECT_GT(diags_.NumErrors(), 0u);
}

TEST_F(ConditionalTest, UnterminatedTakenConditionalReportsOnce) {
  Run("#if 1\nyes\n");  // no matching #endif
  EXPECT_EQ(diags_.NumErrors(), 1u);
}

TEST_F(ConditionalTest, UnterminatedSkippedConditionalReportsOnce) {
  Run("#if 0\nno\n");  // no matching #endif; body is skipped
  EXPECT_EQ(diags_.NumErrors(), 1u);
}

}  // namespace
}  // namespace bcc
