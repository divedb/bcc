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

class FunctionMacroTest : public ::testing::Test {
 protected:
  FileManager fm_;
  SourceManager sm_{fm_};
  HeaderSearch hs_{fm_};
  DiagnosticsEngine diags_{nullptr, &sm_};

  void Main(std::string_view content) {
    sm_.SetMainFileID(sm_.CreateFileID("main.c", std::string(content)));
  }

  std::vector<std::string> ExpandAll(Preprocessor& pp) {
    std::vector<std::string> out;
    for (;;) {
      Token t = pp.Lex();
      if (t.GetKind() == TokenKind::kEOF) break;
      out.push_back(std::string(t.GetLexeme()));
    }
    return out;
  }

  std::vector<std::string> Run(std::string_view content) {
    Main(content);
    Preprocessor pp(sm_, diags_, hs_);
    pp.EnterMainFile();
    return ExpandAll(pp);
  }
};

using V = std::vector<std::string>;

TEST_F(FunctionMacroTest, ExpandsSimpleFunctionLikeMacro) {
  EXPECT_EQ(Run("#define ADD(a, b) a + b\nADD(1, 2)"), (V{"1", "+", "2"}));
}

TEST_F(FunctionMacroTest, NameWithoutParenIsNotExpanded) {
  Main("#define F(a) a\nF");
  Preprocessor pp(sm_, diags_, hs_);
  pp.EnterMainFile();

  Token t = pp.Lex();
  EXPECT_EQ(t.GetKind(), TokenKind::kIdentifier);
  EXPECT_EQ(t.GetLexeme(), "F");
  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kEOF);
}

TEST_F(FunctionMacroTest, NameFollowedByNonParenTokenIsNotExpanded) {
  // 'F' is not a call here; the '+' must survive.
  EXPECT_EQ(Run("#define F(a) a\nF + 1"), (V{"F", "+", "1"}));
}

TEST_F(FunctionMacroTest, PreExpandsArguments) {
  EXPECT_EQ(Run("#define X 5\n#define ID(a) a\nID(X)"), (V{"5"}));
}

TEST_F(FunctionMacroTest, ArgumentWithNestedParensAndCommas) {
  // The comma inside (1, 2) is protected by parentheses -> single argument.
  EXPECT_EQ(Run("#define ID(a) a\nID((1, 2))"), (V{"(", "1", ",", "2", ")"}));
}

TEST_F(FunctionMacroTest, EmptyArgumentExpandsToNothing) {
  EXPECT_EQ(Run("#define ID(a) [a]\nID()"), (V{"[", "]"}));
}

TEST_F(FunctionMacroTest, Stringize) {
  EXPECT_EQ(Run("#define STR(x) #x\nSTR(hello)"), (V{"\"hello\""}));
}

TEST_F(FunctionMacroTest, StringizeJoinsWithSingleSpaces) {
  EXPECT_EQ(Run("#define STR(x) #x\nSTR(a   +   b)"), (V{"\"a + b\""}));
}

TEST_F(FunctionMacroTest, StringizeEscapesQuotesAndBackslashes) {
  EXPECT_EQ(Run("#define STR(x) #x\nSTR(\"a\\b\")"), (V{"\"\\\"a\\\\b\\\"\""}));
}

TEST_F(FunctionMacroTest, PasteIdentifiers) {
  EXPECT_EQ(Run("#define CAT(a, b) a ## b\nCAT(foo, bar)"), (V{"foobar"}));
}

TEST_F(FunctionMacroTest, PasteNumbers) {
  EXPECT_EQ(Run("#define CAT(a, b) a ## b\nCAT(12, 34)"), (V{"1234"}));
}

TEST_F(FunctionMacroTest, PasteResultIsRescannedAsMacro) {
  EXPECT_EQ(Run("#define foobar 99\n#define CAT(a, b) a ## b\nCAT(foo, bar)"),
            (V{"99"}));
}

TEST_F(FunctionMacroTest, PasteWithEmptyOperandActsAsPlacemarker) {
  // b is empty, so `a ## b` yields just the tokens of a.
  EXPECT_EQ(Run("#define J(a, b) a ## b\nJ(x, )"), (V{"x"}));
}

TEST_F(FunctionMacroTest, ObjectLikePaste) {
  EXPECT_EQ(Run("#define J a ## b\nJ"), (V{"ab"}));
}

TEST_F(FunctionMacroTest, VariadicJoinsArgumentsWithCommas) {
  EXPECT_EQ(Run("#define V(...) __VA_ARGS__\nV(1, 2, 3)"),
            (V{"1", ",", "2", ",", "3"}));
}

TEST_F(FunctionMacroTest, VariadicWithNamedParameter) {
  EXPECT_EQ(Run("#define CALL(f, ...) f(__VA_ARGS__)\nCALL(g, 1, 2)"),
            (V{"g", "(", "1", ",", "2", ")"}));
}

TEST_F(FunctionMacroTest, VariadicWithNoTrailingArgs) {
  EXPECT_EQ(Run("#define V(a, ...) a __VA_ARGS__\nV(x)"), (V{"x"}));
}

TEST_F(FunctionMacroTest, RecursiveFunctionLikeMacroTerminates) {
  // f expands to f(0); the inner f is painted (macro disabled) so it stops.
  Main("#define f(x) f(x)\nf(0)");
  Preprocessor pp(sm_, diags_, hs_);
  pp.EnterMainFile();

  EXPECT_EQ(ExpandAll(pp), (V{"f", "(", "0", ")"}));
}

TEST_F(FunctionMacroTest, NestedFunctionLikeInvocation) {
  EXPECT_EQ(Run("#define ID(a) a\n#define ADD(a, b) a + b\nADD(ID(1), ID(2))"),
            (V{"1", "+", "2"}));
}

TEST_F(FunctionMacroTest, FunctionLikeCallSpansWhitespaceBeforeParen) {
  // A function-like macro may be invoked with whitespace before '('.
  EXPECT_EQ(Run("#define F(a) a\nF (7)"), (V{"7"}));
}

TEST_F(FunctionMacroTest, StringizedTokensAreMacroExpansionLocations) {
  Main("#define STR(x) #x\nSTR(hi)");
  Preprocessor pp(sm_, diags_, hs_);
  pp.EnterMainFile();

  Token t = pp.Lex();
  EXPECT_EQ(t.GetKind(), TokenKind::kStringLiteral);
  EXPECT_EQ(t.GetLexeme(), "\"hi\"");
  EXPECT_TRUE(t.GetLocation().IsMacroExpansion());
}

}  // namespace
}  // namespace bcc
