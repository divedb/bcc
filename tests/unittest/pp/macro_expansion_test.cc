#include <string_view>
#include <vector>

#include "bcc/basic/diagnostic.hh"
#include "bcc/basic/file_manager.hh"
#include "bcc/basic/source_manager.hh"
#include "bcc/lex/token_kind.hh"
#include "bcc/pp/header_search.hh"
#include "bcc/pp/identifier_table.hh"
#include "bcc/pp/preprocessor.hh"
#include "gtest/gtest.h"

namespace bcc {
namespace {

class MacroExpansionTest : public ::testing::Test {
 protected:
  FileManager fm_;
  SourceManager sm_{fm_};
  HeaderSearch hs_{fm_};
  DiagnosticsEngine diags_{nullptr, &sm_};

  FileID Main(std::string_view content) {
    FileID fid = sm_.CreateFileID("main.c", std::string(content));
    sm_.SetMainFileID(fid);
    return fid;
  }

  // Lexeme of every token up to (excluding) EOF.
  std::vector<std::string> Expand(Preprocessor& pp) {
    std::vector<std::string> out;
    for (;;) {
      Token t = pp.Lex();
      if (t.GetKind() == TokenKind::kEOF) break;
      out.push_back(std::string(t.GetLexeme()));
    }
    return out;
  }
};

TEST_F(MacroExpansionTest, ExpandsObjectLikeMacro) {
  Main("#define X 1\nX");
  Preprocessor pp(sm_, diags_, hs_);
  pp.EnterMainFile();

  EXPECT_EQ(Expand(pp), (std::vector<std::string>{"1"}));
}

TEST_F(MacroExpansionTest, ExpandsToMultipleTokens) {
  Main("#define TWO 1 2\nTWO");
  Preprocessor pp(sm_, diags_, hs_);
  pp.EnterMainFile();

  EXPECT_EQ(Expand(pp), (std::vector<std::string>{"1", "2"}));
}

TEST_F(MacroExpansionTest, ExpandedTokensCarrySpellingAndExpansionLocations) {
  Main("#define TWO 1 2\nTWO");
  Preprocessor pp(sm_, diags_, hs_);
  pp.EnterMainFile();

  Token t1 = pp.Lex();  // 1
  Token t2 = pp.Lex();  // 2
  ASSERT_EQ(t1.GetLexeme(), "1");
  ASSERT_EQ(t2.GetLexeme(), "2");

  // Both come from a macro expansion.
  EXPECT_TRUE(t1.GetLocation().IsMacroExpansion());
  EXPECT_TRUE(t2.GetLocation().IsMacroExpansion());

  // The expansion point is the TWO invocation on line 2, shared by both.
  SourceLocation e1 = sm_.GetExpansionLoc(t1.GetLocation());
  SourceLocation e2 = sm_.GetExpansionLoc(t2.GetLocation());
  EXPECT_EQ(e1, e2);
  EXPECT_FALSE(e1.IsMacroExpansion());  // resolves to a real file location
  EXPECT_EQ(sm_.GetLine(e1), 2u);

  // The spelling resolves back into the macro body on line 1.
  EXPECT_EQ(sm_.GetLine(sm_.GetSpellingLoc(t1.GetLocation())), 1u);
  EXPECT_EQ(sm_.GetLine(sm_.GetSpellingLoc(t2.GetLocation())), 1u);
}

TEST_F(MacroExpansionTest, RescansForNestedMacros) {
  Main("#define A B\n#define B 42\nA");
  Preprocessor pp(sm_, diags_, hs_);
  pp.EnterMainFile();

  EXPECT_EQ(Expand(pp), (std::vector<std::string>{"42"}));
}

TEST_F(MacroExpansionTest, SelfReferentialMacroExpandsOnce) {
  Main("#define X X\nX");
  Preprocessor pp(sm_, diags_, hs_);
  pp.EnterMainFile();

  Token t = pp.Lex();
  EXPECT_EQ(t.GetKind(), TokenKind::kIdentifier);
  EXPECT_EQ(t.GetLexeme(), "X");
  EXPECT_TRUE(t.IsDisableExpand());  // painted so it never re-expands
  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kEOF);
}

TEST_F(MacroExpansionTest, MutuallyRecursiveMacrosTerminate) {
  Main("#define A B\n#define B A\nA");
  Preprocessor pp(sm_, diags_, hs_);
  pp.EnterMainFile();

  Token t = pp.Lex();
  EXPECT_EQ(t.GetKind(), TokenKind::kIdentifier);
  EXPECT_EQ(t.GetLexeme(), "A");
  EXPECT_TRUE(t.IsDisableExpand());
  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kEOF);
}

TEST_F(MacroExpansionTest, UndefStopsExpansion) {
  Main("#define X 1\n#undef X\nX");
  Preprocessor pp(sm_, diags_, hs_);
  pp.EnterMainFile();

  Token t = pp.Lex();
  EXPECT_EQ(t.GetKind(), TokenKind::kIdentifier);
  EXPECT_EQ(t.GetLexeme(), "X");
  EXPECT_FALSE(t.IsDisableExpand());
}

TEST_F(MacroExpansionTest, EmptyMacroExpandsToNothing) {
  Main("#define E\nE after");
  Preprocessor pp(sm_, diags_, hs_);
  pp.EnterMainFile();

  EXPECT_EQ(Expand(pp), (std::vector<std::string>{"after"}));
}

TEST_F(MacroExpansionTest, RedefinitionUsesLatestDefinition) {
  Main("#define X 1\n#define X 2\nX");
  Preprocessor pp(sm_, diags_, hs_);
  pp.EnterMainFile();

  EXPECT_EQ(Expand(pp), (std::vector<std::string>{"2"}));
}

TEST_F(MacroExpansionTest, TracksMacroDefinitionState) {
  Main("#define X 1\n#undef X\n");
  Preprocessor pp(sm_, diags_, hs_);
  pp.EnterMainFile();

  // Drive the preprocessor to end so both directives are processed.
  while (pp.Lex().GetKind() != TokenKind::kEOF) {
  }

  IdentifierInfo& ii = pp.GetIdentifierTable().Get("X");
  EXPECT_FALSE(pp.IsMacroDefined(&ii));
  EXPECT_EQ(pp.GetMacroInfo(&ii), nullptr);
}

TEST_F(MacroExpansionTest, NonMacroIdentifierIsUnchanged) {
  Main("foo");
  Preprocessor pp(sm_, diags_, hs_);
  pp.EnterMainFile();

  Token t = pp.Lex();
  EXPECT_EQ(t.GetKind(), TokenKind::kIdentifier);
  EXPECT_EQ(t.GetLexeme(), "foo");
  EXPECT_FALSE(t.IsDisableExpand());
}

}  // namespace
}  // namespace bcc
