#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bcc/basic/diagnostic.hh"
#include "bcc/basic/file_manager.hh"
#include "bcc/basic/source_manager.hh"
#include "bcc/pp/header_search.hh"
#include "bcc/pp/preprocessor.hh"
#include "bcc/lex/token_kind.hh"
#include "gtest/gtest.h"

namespace bcc {
namespace {

namespace fs = std::filesystem;

// End-to-end tests for #include resolution, #pragma once, and the include-guard
// multiple-include optimization. Header files are written to a per-test temp
// directory so the real FileManager / SourceManager path is exercised.
class IncludeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const ::testing::TestInfo* info =
        ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = fs::temp_directory_path() /
           (std::string("bcc_include_") + info->name());
    std::error_code ec;
    fs::remove_all(dir_, ec);
    fs::create_directories(dir_);

    fm_ = std::make_unique<FileManager>(FileSystemOptions{dir_.string()});
    sm_ = std::make_unique<SourceManager>(*fm_);
    diags_ = std::make_unique<DiagnosticsEngine>(nullptr, sm_.get());
    hs_ = std::make_unique<HeaderSearch>(*fm_);
    // Angled includes resolve against the temp directory.
    hs_->AddAngledSearchPath(dir_.string());
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  void WriteFile(std::string_view name, std::string_view content) {
    std::ofstream out(dir_ / name);
    out << content;
  }

  // Registers a real on-disk file as the main file so that includer-relative
  // (quoted) lookups have a directory to search.
  void SetMainFile(std::string_view name, std::string_view content) {
    WriteFile(name, content);
    const FileEntry* fe = fm_->GetFile((dir_ / name).string());
    ASSERT_NE(fe, nullptr);
    FileID fid = sm_->CreateFileID(*fe);
    ASSERT_TRUE(fid.IsValid());
    sm_->SetMainFileID(fid);
  }

  std::unique_ptr<Preprocessor> MakePP() {
    auto pp = std::make_unique<Preprocessor>(*sm_, *diags_);
    pp->SetHeaderSearch(*hs_);
    pp->EnterMainFile();
    return pp;
  }

  std::vector<std::string> LexSpellings(Preprocessor& pp) {
    std::vector<std::string> out;
    for (;;) {
      Token t = pp.Lex();
      if (t.GetKind() == TokenKind::kEOF) break;
      out.emplace_back(t.GetLexeme());
    }
    return out;
  }

  fs::path dir_;
  std::unique_ptr<FileManager> fm_;
  std::unique_ptr<SourceManager> sm_;
  std::unique_ptr<DiagnosticsEngine> diags_;
  std::unique_ptr<HeaderSearch> hs_;
};

TEST_F(IncludeTest, ExpandsAngledInclude) {
  WriteFile("foo.h", "int x;\n");
  SetMainFile("main.c", "#include <foo.h>\ndouble y;\n");

  auto pp = MakePP();
  EXPECT_EQ(LexSpellings(*pp),
            (std::vector<std::string>{"int", "x", ";", "double", "y", ";"}));
  EXPECT_FALSE(diags_->HasErrors());
}

TEST_F(IncludeTest, ExpandsQuotedIncludeRelativeToIncluder) {
  WriteFile("bar.h", "char c;\n");
  SetMainFile("main.c", "before\n#include \"bar.h\"\nafter\n");

  auto pp = MakePP();
  EXPECT_EQ(LexSpellings(*pp),
            (std::vector<std::string>{"before", "char", "c", ";", "after"}));
  EXPECT_FALSE(diags_->HasErrors());
}

TEST_F(IncludeTest, PropagatesMacrosAcrossInclude) {
  WriteFile("def.h", "#define N 42\n");
  SetMainFile("main.c", "#include \"def.h\"\nint a = N;\n");

  auto pp = MakePP();
  EXPECT_EQ(LexSpellings(*pp),
            (std::vector<std::string>{"int", "a", "=", "42", ";"}));
}

TEST_F(IncludeTest, ReportsFileNotFound) {
  SetMainFile("main.c", "#include <does_not_exist.h>\nok\n");

  auto pp = MakePP();
  // Preprocessing continues past the bad directive.
  EXPECT_EQ(LexSpellings(*pp), (std::vector<std::string>{"ok"}));
  EXPECT_EQ(diags_->NumErrors(), 1u);
}

TEST_F(IncludeTest, NestedIncludes) {
  WriteFile("a.h", "a1\n#include \"b.h\"\na2\n");
  WriteFile("b.h", "b1\n");
  SetMainFile("main.c", "m0\n#include \"a.h\"\nm1\n");

  auto pp = MakePP();
  EXPECT_EQ(LexSpellings(*pp),
            (std::vector<std::string>{"m0", "a1", "b1", "a2", "m1"}));
}

TEST_F(IncludeTest, PragmaOnceSkipsSecondInclusion) {
  WriteFile("once.h", "#pragma once\nonly\n");
  SetMainFile("main.c",
              "#include \"once.h\"\n#include \"once.h\"\ntail\n");

  auto pp = MakePP();
  // "only" appears just once despite two #includes.
  EXPECT_EQ(LexSpellings(*pp), (std::vector<std::string>{"only", "tail"}));
  EXPECT_FALSE(diags_->HasErrors());
}

TEST_F(IncludeTest, IncludeGuardSkipsSecondInclusion) {
  WriteFile("guard.h",
            "#ifndef GUARD_H\n#define GUARD_H\nbody\n#endif\n");
  SetMainFile("main.c",
              "#include \"guard.h\"\n#include \"guard.h\"\nend\n");

  auto pp = MakePP();
  // The controlling macro GUARD_H is defined on the first inclusion, so the
  // second inclusion is skipped without re-reading the file.
  EXPECT_EQ(LexSpellings(*pp), (std::vector<std::string>{"body", "end"}));
  EXPECT_FALSE(diags_->HasErrors());
}

TEST_F(IncludeTest, NonGuardHeaderIsReReadEachInclusion) {
  // No include guard: the header must be re-read on every inclusion.
  WriteFile("plain.h", "tok\n");
  SetMainFile("main.c",
              "#include \"plain.h\"\n#include \"plain.h\"\n");

  auto pp = MakePP();
  EXPECT_EQ(LexSpellings(*pp), (std::vector<std::string>{"tok", "tok"}));
}

TEST_F(IncludeTest, GuardWithTrailingContentIsNotOptimized) {
  // A token after the guard's #endif breaks the guard shape, so the file is
  // fully re-read on the second inclusion.
  WriteFile("bad.h",
            "#ifndef BAD_H\n#define BAD_H\nbody\n#endif\ntrailer\n");
  SetMainFile("main.c",
              "#include \"bad.h\"\n#include \"bad.h\"\n");

  auto pp = MakePP();
  // First inclusion: body + trailer. Second: guard skips body, trailer remains.
  EXPECT_EQ(LexSpellings(*pp),
            (std::vector<std::string>{"body", "trailer", "trailer"}));
}

TEST_F(IncludeTest, MissingFilenameReportsError) {
  SetMainFile("main.c", "#include\nok\n");

  auto pp = MakePP();
  EXPECT_EQ(LexSpellings(*pp), (std::vector<std::string>{"ok"}));
  EXPECT_EQ(diags_->NumErrors(), 1u);
}

TEST_F(IncludeTest, WorksWithoutHeaderSearchByReportingNotFound) {
  SetMainFile("main.c", "#include <foo.h>\nok\n");

  // Deliberately do not call SetHeaderSearch.
  Preprocessor pp(*sm_, *diags_);
  pp.EnterMainFile();
  std::vector<std::string> out;
  for (;;) {
    Token t = pp.Lex();
    if (t.GetKind() == TokenKind::kEOF) break;
    out.emplace_back(t.GetLexeme());
  }
  EXPECT_EQ(out, (std::vector<std::string>{"ok"}));
  EXPECT_EQ(diags_->NumErrors(), 1u);
}

}  // namespace
}  // namespace bcc
