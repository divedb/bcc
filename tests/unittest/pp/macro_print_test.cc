#include "bcc/pp/macro_print.hh"

#include <map>
#include <memory>
#include <string>

#include "bcc/basic/diagnostic.hh"
#include "bcc/basic/file_manager.hh"
#include "bcc/basic/source_manager.hh"
#include "bcc/lex/token.hh"
#include "bcc/lex/token_kind.hh"
#include "bcc/pp/header_search.hh"
#include "bcc/pp/identifier_table.hh"
#include "bcc/pp/macro_info.hh"
#include "bcc/pp/preprocessor.hh"
#include "gtest/gtest.h"

namespace bcc {
namespace {

// Drives the Preprocessor over a small in-memory TU so FormatMacroDefine is
// exercised against real, parser-built MacroInfo (correct flags and spacing).
class MacroPrintTest : public ::testing::Test {
 protected:
  FileManager fm_;
  SourceManager sm_{fm_};
  HeaderSearch hs_{fm_};
  DiagnosticsEngine diags_{nullptr, &sm_};

  std::unique_ptr<Preprocessor> MakePP(std::string_view src) {
    FileID fid = sm_.CreateFileID("test.c", std::string(src));
    sm_.SetMainFileID(fid);
    auto pp = std::make_unique<Preprocessor>(sm_, diags_, hs_);
    pp->EnterMainFile();
    return pp;
  }

  static void Drain(Preprocessor& pp) {
    for (;;) {
      Token t = pp.Lex();
      if (t.GetKind() == TokenKind::kEOF) break;
    }
  }
};

TEST_F(MacroPrintTest, ObjectLikeWithBody) {
  auto pp = MakePP("#define FOO 42\n");
  Drain(*pp);
  const IdentifierInfo* ii = &pp->GetIdentifierTable().Get("FOO");
  MacroInfo* m = pp->GetMacroInfo(ii);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(FormatMacroDefine(*ii, *m), "#define FOO 42");
}

TEST_F(MacroPrintTest, EmptyObjectLikeHasNoBody) {
  auto pp = MakePP("#define EMPTY\n");
  Drain(*pp);
  const IdentifierInfo* ii = &pp->GetIdentifierTable().Get("EMPTY");
  MacroInfo* m = pp->GetMacroInfo(ii);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(FormatMacroDefine(*ii, *m), "#define EMPTY");
}

TEST_F(MacroPrintTest, FunctionLike) {
  auto pp = MakePP("#define ADD(a, b) ((a) + (b))\n");
  Drain(*pp);
  const IdentifierInfo* ii = &pp->GetIdentifierTable().Get("ADD");
  MacroInfo* m = pp->GetMacroInfo(ii);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(FormatMacroDefine(*ii, *m), "#define ADD(a, b) ((a) + (b))");
}

TEST_F(MacroPrintTest, C99Variadic) {
  auto pp = MakePP("#define LOG(fmt, ...) printf(fmt, __VA_ARGS__)\n");
  Drain(*pp);
  const IdentifierInfo* ii = &pp->GetIdentifierTable().Get("LOG");
  MacroInfo* m = pp->GetMacroInfo(ii);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(FormatMacroDefine(*ii, *m),
            "#define LOG(fmt, ...) printf(fmt, __VA_ARGS__)");
}

TEST_F(MacroPrintTest, ForEachDefinedMacroEnumeratesActiveDefinitions) {
  auto pp = MakePP("#define A 1\n#define B 2\n#define A 10\n");
  Drain(*pp);

  std::map<std::string, std::string> defs;
  pp->ForEachDefinedMacro([&](const IdentifierInfo* name, const MacroInfo* m) {
    defs[std::string(name->GetName())] = FormatMacroDefine(*name, *m);
  });

  // A was redefined; the latest definition wins.
  ASSERT_EQ(defs.count("A"), 1u);
  EXPECT_EQ(defs["A"], "#define A 10");
  ASSERT_EQ(defs.count("B"), 1u);
  EXPECT_EQ(defs["B"], "#define B 2");
}

TEST_F(MacroPrintTest, ForEachDefinedMacroOmitsUndefined) {
  auto pp = MakePP("#define A 1\n#undef A\n");
  Drain(*pp);

  std::map<std::string, std::string> defs;
  pp->ForEachDefinedMacro([&](const IdentifierInfo* name, const MacroInfo* m) {
    defs[std::string(name->GetName())] = FormatMacroDefine(*name, *m);
  });

  EXPECT_EQ(defs.count("A"), 0u);
}

}  // namespace
}  // namespace bcc
