#include <string>
#include <string_view>
#include <vector>

#include "bcc/basic/diagnostic.hh"
#include "bcc/basic/file_manager.hh"
#include "bcc/basic/source_manager.hh"
#include "bcc/lex/token_kind.hh"
#include "bcc/pp/header_search.hh"
#include "bcc/pp/preprocessor.hh"
#include "gtest/gtest.h"

namespace bcc {
namespace {

// Tests for Phase 7: builtin macros (__LINE__ etc.), #line, #error, #warning.
class DirectiveTest : public ::testing::Test {
 protected:
  FileManager fm_;
  SourceManager sm_{fm_};
  HeaderSearch hs_{fm_};
  DiagnosticsEngine diags_{nullptr, &sm_};

  std::vector<Token> LexTokens(std::string_view content,
                               std::string_view name = "main.c") {
    sm_.SetMainFileID(
        sm_.CreateFileID(std::string(name), std::string(content)));
    pp_ = std::make_unique<Preprocessor>(sm_, diags_, hs_);
    pp_->EnterMainFile();

    std::vector<Token> out;
    for (;;) {
      Token t = pp_->Lex();
      if (t.GetKind() == TokenKind::kEOF) break;
      out.push_back(t);
    }
    return out;
  }

  std::vector<std::string> Spellings(std::string_view content,
                                     std::string_view name = "main.c") {
    std::vector<std::string> out;
    for (const Token& t : LexTokens(content, name)) {
      out.emplace_back(t.GetLexeme());
    }
    return out;
  }

  std::unique_ptr<Preprocessor> pp_;
};

using V = std::vector<std::string>;

//===----------------------------------------------------------------------===//
// Builtin macros
//===----------------------------------------------------------------------===//

TEST_F(DirectiveTest, LineBuiltinReportsPhysicalLine) {
  EXPECT_EQ(Spellings("__LINE__\n\n__LINE__\n"), (V{"1", "3"}));
}

TEST_F(DirectiveTest, FileBuiltinIsCurrentFilename) {
  std::vector<Token> toks = LexTokens("__FILE__\n", "widget.c");
  ASSERT_EQ(toks.size(), 1u);
  EXPECT_EQ(toks[0].GetKind(), TokenKind::kStringLiteral);
  EXPECT_EQ(toks[0].GetLexeme(), "\"widget.c\"");
}

TEST_F(DirectiveTest, CounterIncrementsPerUse) {
  EXPECT_EQ(Spellings("__COUNTER__ __COUNTER__ __COUNTER__\n"),
            (V{"0", "1", "2"}));
}

TEST_F(DirectiveTest, IncludeLevelIsZeroInMainFile) {
  EXPECT_EQ(Spellings("__INCLUDE_LEVEL__\n"), (V{"0"}));
}

TEST_F(DirectiveTest, DateAndTimeAreWellFormedStringLiterals) {
  std::vector<Token> toks = LexTokens("__DATE__ __TIME__\n");
  ASSERT_EQ(toks.size(), 2u);
  // __DATE__ is "Mmm dd yyyy" (11 chars) plus quotes.
  EXPECT_EQ(toks[0].GetKind(), TokenKind::kStringLiteral);
  EXPECT_EQ(toks[0].GetLexeme().size(), 13u);
  // __TIME__ is "hh:mm:ss" (8 chars) plus quotes.
  EXPECT_EQ(toks[1].GetKind(), TokenKind::kStringLiteral);
  EXPECT_EQ(toks[1].GetLexeme().size(), 10u);
}

TEST_F(DirectiveTest, LineBuiltinUsesInvocationSiteInsideMacro) {
  // __LINE__ in a macro body reports where the macro is used, not defined.
  EXPECT_EQ(Spellings("#define HERE __LINE__\nHERE\nHERE\n"), (V{"2", "3"}));
}

//===----------------------------------------------------------------------===//
// #line
//===----------------------------------------------------------------------===//

TEST_F(DirectiveTest, LineDirectiveOverridesLineNumber) {
  EXPECT_EQ(Spellings("#line 100\n__LINE__\n__LINE__\n"), (V{"100", "101"}));
}

TEST_F(DirectiveTest, LineDirectiveOverridesFilename) {
  std::vector<Token> toks =
      LexTokens("#line 10 \"gen.y\"\n__FILE__ __LINE__\n");
  ASSERT_EQ(toks.size(), 2u);
  EXPECT_EQ(toks[0].GetLexeme(), "\"gen.y\"");
  EXPECT_EQ(toks[1].GetLexeme(), "10");
}

TEST_F(DirectiveTest, LineDirectiveArgumentIsMacroExpanded) {
  EXPECT_EQ(Spellings("#define N 7\n#line N\n__LINE__\n"), (V{"7"}));
}

TEST_F(DirectiveTest, LineDirectiveRejectsNonInteger) {
  EXPECT_EQ(Spellings("#line foo\nok\n"), (V{"ok"}));
  EXPECT_EQ(diags_.NumErrors(), 1u);
}

//===----------------------------------------------------------------------===//
// #error / #warning
//===----------------------------------------------------------------------===//

TEST_F(DirectiveTest, ErrorDirectiveReportsError) {
  Spellings("#error something went wrong\n");
  EXPECT_EQ(diags_.NumErrors(), 1u);
}

TEST_F(DirectiveTest, WarningDirectiveReportsWarningNotError) {
  Spellings("#warning be careful\n");
  EXPECT_EQ(diags_.NumErrors(), 0u);
  EXPECT_EQ(diags_.NumWarnings(), 1u);
}

TEST_F(DirectiveTest, ErrorInSkippedBranchIsInert) {
  EXPECT_EQ(Spellings("#if 0\n#error nope\n#endif\nok\n"), (V{"ok"}));
  EXPECT_EQ(diags_.NumErrors(), 0u);
}

//===----------------------------------------------------------------------===//
// New builtin macros (P0)
//===----------------------------------------------------------------------===//

TEST_F(DirectiveTest, StdcBuiltinIsOne) {
  EXPECT_EQ(Spellings("__STDC__\n"), (V{"1"}));
}

TEST_F(DirectiveTest, StdcHostedBuiltinIsOne) {
  EXPECT_EQ(Spellings("__STDC_HOSTED__\n"), (V{"1"}));
}

TEST_F(DirectiveTest, StdcVersionIsC11) {
  EXPECT_EQ(Spellings("__STDC_VERSION__\n"), (V{"201112L"}));
}

TEST_F(DirectiveTest, BaseFileBuiltinIsMainFileName) {
  std::vector<Token> toks = LexTokens("__BASE_FILE__\n", "mymain.c");
  ASSERT_EQ(toks.size(), 1u);
  EXPECT_EQ(toks[0].GetKind(), TokenKind::kStringLiteral);
  EXPECT_EQ(toks[0].GetLexeme(), "\"mymain.c\"");
}

TEST_F(DirectiveTest, FileNameBuiltinIsLastComponent) {
  std::vector<Token> toks =
      LexTokens("__FILE_NAME__\n", "/home/user/src/main.c");
  ASSERT_EQ(toks.size(), 1u);
  EXPECT_EQ(toks[0].GetKind(), TokenKind::kStringLiteral);
  EXPECT_EQ(toks[0].GetLexeme(), "\"main.c\"");
}

TEST_F(DirectiveTest, FileNameBuiltinNoPathReturnsFull) {
  std::vector<Token> toks = LexTokens("__FILE_NAME__\n", "no_dir.c");
  ASSERT_EQ(toks.size(), 1u);
  EXPECT_EQ(toks[0].GetLexeme(), "\"no_dir.c\"");
}

TEST_F(DirectiveTest, TimestampBuiltinIsWellFormedStringLiteral) {
  std::vector<Token> toks = LexTokens("__TIMESTAMP__\n");
  ASSERT_EQ(toks.size(), 1u);
  EXPECT_EQ(toks[0].GetKind(), TokenKind::kStringLiteral);
  // __TIMESTAMP__ is "Thu Jan  1 00:00:00 1970" style (24 chars) plus quotes.
  EXPECT_EQ(toks[0].GetLexeme().size(), 26u);
}

TEST_F(DirectiveTest, FltEvalMethodBuiltinIsZero) {
  EXPECT_EQ(Spellings("__FLT_EVAL_METHOD__\n"), (V{"0"}));
}

TEST_F(DirectiveTest, PragmaOperatorWithoutCallIsIdentifier) {
  // A bare _Pragma (not followed by '(') is left as an identifier token.
  std::vector<Token> toks = LexTokens("_Pragma\n");
  ASSERT_GE(toks.size(), 1u);
  EXPECT_EQ(toks[0].GetKind(), TokenKind::kIdentifier);
  EXPECT_EQ(toks[0].GetLexeme(), "_Pragma");
}

TEST_F(DirectiveTest, PragmaOperatorIsProcessed) {
  // _Pragma("mark ...") is processed as a #pragma mark directive, which is
  // consumed entirely (no tokens survive into the output stream).
  EXPECT_EQ(Spellings("_Pragma(\"mark hi\")\nok\n"), (V{"ok"}));
}

//===----------------------------------------------------------------------===//
// Poison pragma
//===----------------------------------------------------------------------===//

TEST_F(DirectiveTest, PoisonIdentifierReportsError) {
  Spellings("#pragma GCC poison foo\nfoo\n");
  EXPECT_GE(diags_.NumErrors(), 1u);
}

TEST_F(DirectiveTest, PoisonNonIdentifierReportsError) {
  Spellings("#pragma GCC poison 123\n");
  EXPECT_GE(diags_.NumErrors(), 1u);
}

TEST_F(DirectiveTest, PoisonMultipleIdentifiers) {
  Spellings("#pragma GCC poison foo bar baz\nfoo bar baz\n");
  EXPECT_GE(diags_.NumErrors(), 3u);
}

TEST_F(DirectiveTest, PoisonAfterDefineWarns) {
  Spellings("#define FOO 1\n#pragma GCC poison FOO\n");
  EXPECT_GE(diags_.NumWarnings(), 1u);
}

TEST_F(DirectiveTest, PoisonDoesNotReportOnNonIdentifierTokens) {
  Spellings("#pragma GCC poison foo\nint x = 1;\n");
  // Only the 'foo' usage would trigger an error; 'int', 'x', '=', '1', ';' are
  // fine. Since foo is not used, no errors.
  EXPECT_EQ(diags_.NumErrors(), 0u);
}

//===----------------------------------------------------------------------===//
// #ident / #sccs directive
//===----------------------------------------------------------------------===//

TEST_F(DirectiveTest, IdentDirectiveIsSilentlyIgnored) {
  EXPECT_EQ(Spellings("#ident \"string\"\nok\n"), (V{"ok"}));
  EXPECT_EQ(diags_.NumErrors(), 0u);
}

TEST_F(DirectiveTest, SccsDirectiveIsSilentlyIgnored) {
  EXPECT_EQ(Spellings("#sccs \"string\"\nok\n"), (V{"ok"}));
  EXPECT_EQ(diags_.NumErrors(), 0u);
}

//===----------------------------------------------------------------------===//
// push_macro / pop_macro pragma
//===----------------------------------------------------------------------===//

TEST_F(DirectiveTest, PushMacroSavesAndPopMacroRestores) {
  // Define a macro, push it, undefine it, pop it — the definition should
  // return.
  std::vector<Token> toks = LexTokens(
      "#define X 1\n"
      "#pragma push_macro(\"X\")\n"
      "#undef X\n"
      "#pragma pop_macro(\"X\")\n"
      "X\n");
  ASSERT_EQ(toks.size(), 1u);
  EXPECT_EQ(toks[0].GetLexeme(), "1");
}

TEST_F(DirectiveTest, PushMacroMissingArgReportsError) {
  Spellings("#pragma push_macro\n");
  EXPECT_GE(diags_.NumErrors(), 1u);
}

TEST_F(DirectiveTest, PopMacroMissingArgReportsError) {
  Spellings("#pragma pop_macro\n");
  EXPECT_GE(diags_.NumErrors(), 1u);
}

TEST_F(DirectiveTest, ClangNamespacePoisonWorks) {
  Spellings("#pragma clang poison bar\nbar\n");
  EXPECT_GE(diags_.NumErrors(), 1u);
}

//===----------------------------------------------------------------------===//
// #pragma STDC
//===----------------------------------------------------------------------===//

TEST_F(DirectiveTest, STdcPragmaIsSilentlyAccepted) {
  EXPECT_EQ(Spellings("#pragma STDC FENV_ACCESS ON\nok\n"), (V{"ok"}));
  EXPECT_EQ(Spellings("#pragma STDC FP_CONTRACT OFF\nok\n"), (V{"ok"}));
  EXPECT_EQ(Spellings("#pragma STDC CX_LIMITED_RANGE DEFAULT\nok\n"),
            (V{"ok"}));
}

//===----------------------------------------------------------------------===//
// #pragma GCC system_header / dependency / diagnostic
//===----------------------------------------------------------------------===//

TEST_F(DirectiveTest, GccSystemHeaderPragmaIsSilentlyAccepted) {
  EXPECT_EQ(Spellings("#pragma GCC system_header\nok\n"), (V{"ok"}));
  EXPECT_EQ(diags_.NumErrors(), 0u);
}

TEST_F(DirectiveTest, GccDependencyPragmaIsSilentlyAccepted) {
  // We can't easily test the full timestamp comparison, but we can verify
  // it doesn't crash and accepts the syntax.
  EXPECT_EQ(Spellings("#pragma GCC dependency \"nonexistent.h\"\nok\n"),
            (V{"ok"}));
  EXPECT_EQ(diags_.NumErrors(), 0u);
}

TEST_F(DirectiveTest, GccDiagnosticPragmaIsReemitted) {
  // Like Clang's `clang -E -P`, #pragma GCC diagnostic survives into the
  // preprocessed output (it is not silently consumed).
  EXPECT_EQ(Spellings("#pragma GCC diagnostic push\nok\n"),
            (V{"#", "pragma", "GCC", "diagnostic", "push", "ok"}));
  EXPECT_EQ(Spellings("#pragma GCC diagnostic pop\nok\n"),
            (V{"#", "pragma", "GCC", "diagnostic", "pop", "ok"}));
}

}  // namespace
}  // namespace bcc
