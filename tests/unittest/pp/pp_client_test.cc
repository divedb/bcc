// Tests for the Phase 8 client surface: PPCallbacks and lookahead/backtracking.

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bcc/basic/diagnostic.hh"
#include "bcc/basic/file_manager.hh"
#include "bcc/basic/source_manager.hh"
#include "bcc/lex/token.hh"
#include "bcc/lex/token_kind.hh"
#include "bcc/pp/header_search.hh"
#include "bcc/pp/identifier_table.hh"
#include "bcc/pp/pp_callbacks.hh"
#include "bcc/pp/preprocessor.hh"
#include "gtest/gtest.h"

namespace bcc {
namespace {

// Records preprocessor events as human-readable strings for assertions.
class RecordingCallbacks : public PPCallbacks {
 public:
  explicit RecordingCallbacks(std::vector<std::string>& log) : log_(log) {}

  void FileChanged(SourceLocation, FileChangeReason reason, FileID,
                   CharacteristicKind) override {
    log_.push_back(reason == FileChangeReason::kEnterFile ? "enter" : "exit");
  }
  void InclusionDirective(SourceLocation, std::string_view filename,
                          bool is_angled, const FileEntry* file,
                          CharacteristicKind) override {
    log_.push_back(std::string("include ") + (is_angled ? "<" : "\"") +
                   std::string(filename) + (file ? " found" : " missing"));
  }
  void MacroDefined(const IdentifierInfo* name, const MacroInfo*) override {
    log_.push_back("define " + std::string(name->GetName()));
  }
  void MacroUndefined(const IdentifierInfo* name) override {
    log_.push_back("undef " + std::string(name->GetName()));
  }
  void MacroExpands(const Token& name, const MacroInfo*) override {
    log_.push_back("expand " + std::string(name.GetLexeme()));
  }
  void If(SourceLocation, bool condition) override {
    log_.push_back(condition ? "if true" : "if false");
  }
  void Ifdef(SourceLocation, const IdentifierInfo* name,
             bool defined) override {
    log_.push_back("ifdef " + std::string(name->GetName()) +
                   (defined ? " y" : " n"));
  }
  void Ifndef(SourceLocation, const IdentifierInfo* name,
              bool defined) override {
    log_.push_back("ifndef " + std::string(name->GetName()) +
                   (defined ? " y" : " n"));
  }
  void Else(SourceLocation) override { log_.push_back("else"); }
  void Endif(SourceLocation) override { log_.push_back("endif"); }
  void PragmaDirective(SourceLocation) override { log_.push_back("pragma"); }

 private:
  std::vector<std::string>& log_;
};

class PPClientTest : public ::testing::Test {
 protected:
  FileManager fm_;
  SourceManager sm_{fm_};
  HeaderSearch hs_{fm_};
  DiagnosticsEngine diags_{nullptr, &sm_};

  std::vector<std::string> events_;

  // Builds a preprocessor over the given content, with recording callbacks
  // installed, and drains the whole token stream.
  void RunWithCallbacks(std::string_view content) {
    sm_.SetMainFileID(sm_.CreateFileID("main.c", std::string(content)));
    Preprocessor pp(sm_, diags_, hs_);
    pp.SetPPCallbacks(std::make_unique<RecordingCallbacks>(events_));
    pp.EnterMainFile();
    while (pp.Lex().GetKind() != TokenKind::kEOF) {
    }
  }

  // Builds a preprocessor and leaves it ready for manual Lex/LookAhead calls.
  std::unique_ptr<Preprocessor> Make(std::string_view content) {
    sm_.SetMainFileID(sm_.CreateFileID("main.c", std::string(content)));
    auto pp = std::make_unique<Preprocessor>(sm_, diags_, hs_);
    pp->EnterMainFile();
    return pp;
  }
};

//===----------------------------------------------------------------------===//
// PPCallbacks
//===----------------------------------------------------------------------===//

TEST_F(PPClientTest, ReportsMacroDefineUndefAndExpand) {
  RunWithCallbacks("#define FOO 1\nFOO\n#undef FOO\n");
  EXPECT_EQ(events_, (std::vector<std::string>{"enter", "define FOO",
                                               "expand FOO", "undef FOO"}));
}

TEST_F(PPClientTest, ReportsConditionalEvents) {
  RunWithCallbacks("#if 1\na\n#endif\n#ifdef X\nb\n#endif\n");
  EXPECT_EQ(events_, (std::vector<std::string>{"enter", "if true", "endif",
                                               "ifdef X n", "endif"}));
}

TEST_F(PPClientTest, ReportsIfndefAndElse) {
  RunWithCallbacks("#ifndef X\na\n#else\nb\n#endif\n");
  EXPECT_EQ(events_,
            (std::vector<std::string>{"enter", "ifndef X n", "else", "endif"}));
}

TEST_F(PPClientTest, ReportsInclusionAndPragma) {
  // No HeaderSearch is configured, so the include resolves to "missing" but the
  // callback still fires; #pragma fires regardless.
  RunWithCallbacks("#include <sys.h>\n#pragma once\n");
  EXPECT_EQ(events_, (std::vector<std::string>{
                         "enter", "include <sys.h missing", "pragma"}));
}

//===----------------------------------------------------------------------===//
// Lookahead
//===----------------------------------------------------------------------===//

TEST_F(PPClientTest, LookAheadDoesNotConsume) {
  auto pp = Make("a b c\n");
  EXPECT_EQ(pp->LookAhead(0).GetLexeme(), "a");
  EXPECT_EQ(pp->LookAhead(1).GetLexeme(), "b");
  EXPECT_EQ(pp->LookAhead(2).GetLexeme(), "c");
  // The stream is unaffected by the peeks.
  EXPECT_EQ(pp->Lex().GetLexeme(), "a");
  EXPECT_EQ(pp->Lex().GetLexeme(), "b");
  EXPECT_EQ(pp->Lex().GetLexeme(), "c");
  EXPECT_EQ(pp->Lex().GetKind(), TokenKind::kEOF);
}

//===----------------------------------------------------------------------===//
// Backtracking
//===----------------------------------------------------------------------===//

TEST_F(PPClientTest, BacktrackRewindsToMark) {
  auto pp = Make("a b c\n");
  pp->EnableBacktrackAtThisPos();
  EXPECT_EQ(pp->Lex().GetLexeme(), "a");
  EXPECT_EQ(pp->Lex().GetLexeme(), "b");
  pp->Backtrack();
  // Re-reads from the mark.
  EXPECT_EQ(pp->Lex().GetLexeme(), "a");
  EXPECT_EQ(pp->Lex().GetLexeme(), "b");
  EXPECT_EQ(pp->Lex().GetLexeme(), "c");
}

TEST_F(PPClientTest, CommitKeepsConsumedPosition) {
  auto pp = Make("a b c\n");
  pp->EnableBacktrackAtThisPos();
  EXPECT_EQ(pp->Lex().GetLexeme(), "a");
  pp->CommitBacktrackedTokens();
  EXPECT_FALSE(pp->IsBacktrackEnabled());
  // No rewind: continues after 'a'.
  EXPECT_EQ(pp->Lex().GetLexeme(), "b");
  EXPECT_EQ(pp->Lex().GetLexeme(), "c");
}

TEST_F(PPClientTest, NestedBacktrackReturnsToInnerThenOuter) {
  auto pp = Make("a b c d\n");
  pp->EnableBacktrackAtThisPos();  // outer at 'a'
  EXPECT_EQ(pp->Lex().GetLexeme(), "a");
  pp->EnableBacktrackAtThisPos();  // inner at 'b'
  EXPECT_EQ(pp->Lex().GetLexeme(), "b");
  pp->Backtrack();  // back to 'b'
  EXPECT_EQ(pp->Lex().GetLexeme(), "b");
  EXPECT_EQ(pp->Lex().GetLexeme(), "c");
  pp->Backtrack();  // back to 'a'
  EXPECT_EQ(pp->Lex().GetLexeme(), "a");
  EXPECT_EQ(pp->Lex().GetLexeme(), "b");
}

TEST_F(PPClientTest, BacktrackAcrossMacroExpansion) {
  // Backtracking must replay the expanded token stream, not re-run expansion.
  auto pp = Make("#define TWO 1 + 1\nTWO x\n");
  pp->EnableBacktrackAtThisPos();
  EXPECT_EQ(pp->Lex().GetLexeme(), "1");
  EXPECT_EQ(pp->Lex().GetLexeme(), "+");
  EXPECT_EQ(pp->Lex().GetLexeme(), "1");
  EXPECT_EQ(pp->Lex().GetLexeme(), "x");
  pp->Backtrack();
  EXPECT_EQ(pp->Lex().GetLexeme(), "1");
  EXPECT_EQ(pp->Lex().GetLexeme(), "+");
  EXPECT_EQ(pp->Lex().GetLexeme(), "1");
  EXPECT_EQ(pp->Lex().GetLexeme(), "x");
  EXPECT_EQ(pp->Lex().GetKind(), TokenKind::kEOF);
}

}  // namespace
}  // namespace bcc
