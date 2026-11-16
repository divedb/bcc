#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bcc/basic/diagnostic.hh"
#include "bcc/basic/file_manager.hh"
#include "bcc/basic/source_manager.hh"
#include "bcc/lex/token_kind.hh"
#include "bcc/pp/header_search.hh"
#include "bcc/pp/pp_callbacks.hh"
#include "bcc/pp/preprocessor.hh"
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
    builtin_dir_ = fs::path(dir_.string() + "_builtin");
    std::error_code ec;
    fs::remove_all(dir_, ec);
    fs::remove_all(builtin_dir_, ec);
    fs::create_directories(dir_);
    fs::create_directories(builtin_dir_);

    fm_ = std::make_unique<FileManager>(FileSystemOptions{dir_.string()});
    sm_ = std::make_unique<SourceManager>(*fm_);
    diags_ = std::make_unique<DiagnosticsEngine>(nullptr, sm_.get());
    hs_ = std::make_unique<HeaderSearch>(*fm_);
    // The built-in tier precedes the angled tier (see AddBuiltinSearchPath);
    // both resolve against per-test temp directories.
    hs_->AddBuiltinSearchPath(builtin_dir_.string());
#ifdef BCC_BUILTIN_HEADERS_DIR
    // The compiler's real built-in headers follow the synthetic ones.
    hs_->AddBuiltinSearchPath(BCC_BUILTIN_HEADERS_DIR);
#endif
    // Angled includes resolve against the temp directory.
    hs_->AddAngledSearchPath(dir_.string());
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
    fs::remove_all(builtin_dir_, ec);
  }

  void WriteFile(std::string_view name, std::string_view content) {
    std::ofstream out(dir_ / name);
    out << content;
  }

  // Writes a header into the built-in tier directory.
  void WriteBuiltinFile(std::string_view name, std::string_view content) {
    std::ofstream out(builtin_dir_ / name);
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
    auto pp = std::make_unique<Preprocessor>(*sm_, *diags_, *hs_);
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
  fs::path builtin_dir_;
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

TEST_F(IncludeTest, ResolvesLineSplicedHeaderName) {
  WriteFile("foobar.h", "spliced\n");
  SetMainFile("main.c", "#include <foo\\\nbar.h>\nafter\n");

  auto pp = MakePP();
  EXPECT_EQ(LexSpellings(*pp), (std::vector<std::string>{"spliced", "after"}));
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
  SetMainFile("main.c", "#include \"once.h\"\n#include \"once.h\"\ntail\n");

  auto pp = MakePP();
  // "only" appears just once despite two #includes.
  EXPECT_EQ(LexSpellings(*pp), (std::vector<std::string>{"only", "tail"}));
  EXPECT_FALSE(diags_->HasErrors());
}

TEST_F(IncludeTest, IncludeGuardSkipsSecondInclusion) {
  WriteFile("guard.h", "#ifndef GUARD_H\n#define GUARD_H\nbody\n#endif\n");
  SetMainFile("main.c", "#include \"guard.h\"\n#include \"guard.h\"\nend\n");

  auto pp = MakePP();
  // The controlling macro GUARD_H is defined on the first inclusion, so the
  // second inclusion is skipped without re-reading the file.
  EXPECT_EQ(LexSpellings(*pp), (std::vector<std::string>{"body", "end"}));
  EXPECT_FALSE(diags_->HasErrors());
}

TEST_F(IncludeTest, ImportHonorsPragmaOnceFromEarlierInclude) {
  WriteFile("once.h", "#pragma once\nonly\n");
  SetMainFile("main.c", "#include \"once.h\"\n#import \"once.h\"\ntail\n");

  auto pp = MakePP();
  EXPECT_EQ(LexSpellings(*pp), (std::vector<std::string>{"only", "tail"}));
  EXPECT_FALSE(diags_->HasErrors());
}

TEST_F(IncludeTest, ImportHonorsActiveControllingMacro) {
  WriteFile("guard.h", "#ifndef GUARD_H\n#define GUARD_H\nbody\n#endif\n");
  SetMainFile("main.c", "#include \"guard.h\"\n#import \"guard.h\"\ntail\n");

  auto pp = MakePP();
  EXPECT_EQ(LexSpellings(*pp), (std::vector<std::string>{"body", "tail"}));
  EXPECT_FALSE(diags_->HasErrors());
}

TEST_F(IncludeTest, NonGuardHeaderIsReReadEachInclusion) {
  // No include guard: the header must be re-read on every inclusion.
  WriteFile("plain.h", "tok\n");
  SetMainFile("main.c", "#include \"plain.h\"\n#include \"plain.h\"\n");

  auto pp = MakePP();
  EXPECT_EQ(LexSpellings(*pp), (std::vector<std::string>{"tok", "tok"}));
}

TEST_F(IncludeTest, GuardWithTrailingContentIsNotOptimized) {
  // A token after the guard's #endif breaks the guard shape, so the file is
  // fully re-read on the second inclusion.
  WriteFile("bad.h", "#ifndef BAD_H\n#define BAD_H\nbody\n#endif\ntrailer\n");
  SetMainFile("main.c", "#include \"bad.h\"\n#include \"bad.h\"\n");

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

TEST_F(IncludeTest, EmptyHeaderSearchReportsNotFound) {
  SetMainFile("main.c", "#include <foo.h>\nok\n");

  // HeaderSearch is always present; no matching file exists in its paths.
  Preprocessor pp(*sm_, *diags_, *hs_);
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

// Records the system-header characteristic reported for #include resolution and
// file-enter/exit transitions. Installed after EnterMainFile so only the
// included file's transitions are captured (not the main file's own enter).
class CharacteristicCallbacks : public PPCallbacks {
 public:
  CharacteristicKind last_inclusion = CharacteristicKind::kUser;
  std::vector<CharacteristicKind> entered;
  std::vector<CharacteristicKind> exited;

  void FileChanged(SourceLocation, FileChangeReason reason, FileID,
                   CharacteristicKind file_type) override {
    if (reason == FileChangeReason::kEnterFile) {
      entered.push_back(file_type);
    } else {
      exited.push_back(file_type);
    }
  }
  void InclusionDirective(SourceLocation, std::string_view, bool,
                          const FileEntry*,
                          CharacteristicKind file_type) override {
    last_inclusion = file_type;
  }
};

// A header found on the angled (system) search path is reported as a system
// header both at #include resolution and on file entry; exiting it returns to
// the user-coded main file.
TEST_F(IncludeTest, AngledIncludeIsSystemHeader) {
  WriteFile("foo.h", "int x;\n");
  SetMainFile("main.c", "#include <foo.h>\n");

  auto pp = MakePP();
  auto cb = std::make_unique<CharacteristicCallbacks>();
  CharacteristicCallbacks* raw = cb.get();
  pp->SetPPCallbacks(std::move(cb));

  LexSpellings(*pp);

  EXPECT_EQ(raw->last_inclusion, CharacteristicKind::kSystem);
  ASSERT_EQ(raw->entered.size(), 1u);
  EXPECT_EQ(raw->entered.front(), CharacteristicKind::kSystem);
  ASSERT_EQ(raw->exited.size(), 1u);
  // Resuming the main file reports a user characteristic.
  EXPECT_EQ(raw->exited.front(), CharacteristicKind::kUser);
}

// A quoted include resolved relative to the includer is user code throughout.
TEST_F(IncludeTest, QuotedIncludeIsUserHeader) {
  WriteFile("bar.h", "char c;\n");
  SetMainFile("main.c", "#include \"bar.h\"\n");

  auto pp = MakePP();
  auto cb = std::make_unique<CharacteristicCallbacks>();
  CharacteristicCallbacks* raw = cb.get();
  pp->SetPPCallbacks(std::move(cb));

  LexSpellings(*pp);

  EXPECT_EQ(raw->last_inclusion, CharacteristicKind::kUser);
  for (CharacteristicKind ck : raw->entered) {
    EXPECT_EQ(ck, CharacteristicKind::kUser);
  }
}

// `#pragma GCC system_header` promotes the current file to a system header:
// IsInSystemHeader() reflects it while the file is being lexed, and the
// promotion is recorded in HeaderSearch for later lookups.
TEST_F(IncludeTest, PragmaSystemHeaderPromotesCurrentFile) {
  WriteFile("sys.h", "#pragma GCC system_header\nbody\n");
  SetMainFile("main.c", "#include \"sys.h\"\n");

  auto pp = MakePP();

  // The header resolves via the includer directory, so at #include time it is
  // still user code.
  bool in_system_for_body = false;
  for (;;) {
    Token t = pp->Lex();
    if (t.GetKind() == TokenKind::kEOF) break;
    // The only emitted token is "body", which follows the pragma inside sys.h.
    in_system_for_body = pp->IsInSystemHeader();
  }
  EXPECT_TRUE(in_system_for_body);

  // The pragma persisted the system characteristic on the file's record.
  const FileEntry* fe = fm_->GetFile((dir_ / "sys.h").string());
  ASSERT_NE(fe, nullptr);
  EXPECT_EQ(hs_->GetFileCharacteristic(fe), CharacteristicKind::kSystem);
  EXPECT_FALSE(diags_->HasErrors());
}

// The built-in tier is searched before the angled (-I) directories: a header
// found there wins over a same-named file on the angled path.
TEST_F(IncludeTest, BuiltinTierPrecedesAngledDirs) {
  WriteBuiltinFile("stddef_like.h", "int builtin;\n");
  WriteFile("stddef_like.h", "int angled;\n");
  SetMainFile("main.c", "#include <stddef_like.h>\n");

  auto pp = MakePP();
  EXPECT_EQ(LexSpellings(*pp), (std::vector<std::string>{"int", "builtin", ";"}));
  EXPECT_FALSE(diags_->HasErrors());
}

// The quoted tier (includer-relative lookup) still overrides the built-in
// tier, so user code can deliberately shadow a built-in header.
TEST_F(IncludeTest, QuotedIncludeOverridesBuiltinTier) {
  WriteBuiltinFile("stddef_like.h", "int builtin;\n");
  WriteFile("stddef_like.h", "int local;\n");
  SetMainFile("main.c", "#include \"stddef_like.h\"\n");

  auto pp = MakePP();
  EXPECT_EQ(LexSpellings(*pp), (std::vector<std::string>{"int", "local", ";"}));
  EXPECT_FALSE(diags_->HasErrors());
}

// A header found in the built-in tier reports the system characteristic,
// like one found on the angled path.
TEST_F(IncludeTest, BuiltinHeaderHasSystemCharacteristic) {
  WriteBuiltinFile("stddef_like.h", "int builtin;\n");
  SetMainFile("main.c", "#include <stddef_like.h>\n");

  auto pp = MakePP();
  EXPECT_EQ(LexSpellings(*pp), (std::vector<std::string>{"int", "builtin", ";"}));

  const FileEntry* fe = fm_->GetFile((builtin_dir_ / "stddef_like.h").string());
  ASSERT_NE(fe, nullptr);
  EXPECT_EQ(hs_->GetFileCharacteristic(fe), CharacteristicKind::kSystem);
}

// #include_next from a built-in header resumes with the angled directories,
// mirroring how the compiler resource directory precedes system directories.
TEST_F(IncludeTest, IncludeNextFromBuiltinContinuesIntoAngled) {
  WriteBuiltinFile("chain.h", "from_builtin\n#include_next <chain.h>\n");
  WriteFile("chain.h", "from_angled\n");
  SetMainFile("main.c", "#include <chain.h>\n");

  auto pp = MakePP();
  EXPECT_EQ(LexSpellings(*pp),
            (std::vector<std::string>{"from_builtin", "from_angled"}));
  EXPECT_FALSE(diags_->HasErrors());
}

// A built-in header without an #include_next still resolves normally when the
// same name also exists in a later tier.
TEST_F(IncludeTest, AngledHeaderReachableWhenNotShadowed) {
  WriteBuiltinFile("only_builtin.h", "b\n");
  WriteFile("only_angled.h", "a\n");
  SetMainFile("main.c", "#include <only_angled.h>\n");

  auto pp = MakePP();
  EXPECT_EQ(LexSpellings(*pp), (std::vector<std::string>{"a"}));
  EXPECT_FALSE(diags_->HasErrors());
}

#ifdef BCC_BUILTIN_HEADERS_DIR

// The compiler's real built-in headers resolve from the configured built-in
// directory, enter cleanly, and record the system characteristic.
TEST_F(IncludeTest, RealBuiltinHeadersResolve) {
  SetMainFile("main.c", "#include <stddef.h>\nint ok;\n");

  auto pp = MakePP();
  LexSpellings(*pp);  // drain
  EXPECT_FALSE(diags_->HasErrors());

  const FileEntry* fe =
      fm_->GetFile(std::string(BCC_BUILTIN_HEADERS_DIR) + "/stddef.h");
  ASSERT_NE(fe, nullptr);
  EXPECT_EQ(hs_->GetFileCharacteristic(fe), CharacteristicKind::kSystem);
}

// Partial (__need_) mode of <stddef.h>: only the requested definition's
// tokens are produced, and the request macro is consumed (#undef'd).
TEST_F(IncludeTest, RealBuiltinStddefNeedProtocol) {
  SetMainFile("main.c",
              "#define __need_size_t\n"
              "#include <stddef.h>\n"
              "int ok;\n");

  auto pp = MakePP();
  EXPECT_EQ(LexSpellings(*pp),
            (std::vector<std::string>{"typedef", "unsigned", "long", "size_t",
                                      ";", "int", "ok", ";"}));
  EXPECT_FALSE(diags_->HasErrors());
}

#endif  // BCC_BUILTIN_HEADERS_DIR

}  // namespace
}  // namespace bcc
