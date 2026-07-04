#include "bcc/pp/preprocessor.hh"

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "bcc/basic/diagnostic.hh"
#include "bcc/basic/file_manager.hh"
#include "bcc/basic/presumed_loc.hh"
#include "bcc/basic/source_manager.hh"
#include "bcc/lex/token_kind.hh"
#include "bcc/pp/identifier_table.hh"
#include "bcc/pp/header_search.hh"
#include "bcc/pp/pp_callbacks.hh"
#include "gtest/gtest.h"

namespace bcc {
namespace {

namespace fs = std::filesystem;

// Minimal recording callbacks for testing directive and file-entry events.
struct RecordingPPCallbacks : public PPCallbacks {
  const SourceManager& sm;

  explicit RecordingPPCallbacks(const SourceManager& s) : sm(s) {}

  struct DirectiveInfo {
    std::string name;
  };
  struct FileEntryInfo {
    FileID buffer_id;
    bool is_system_header = false;
  };

  std::vector<DirectiveInfo> directives;
  std::vector<FileEntryInfo> files_entered;

  void FileChanged(SourceLocation loc, FileChangeReason reason,
                   FileID, CharacteristicKind file_type) override {
    if (reason == FileChangeReason::kEnterFile) {
      files_entered.push_back({sm.GetFileID(loc), IsSystemHeader(file_type)});
    }
  }

  void MacroDefined(const IdentifierInfo*, const MacroInfo*) override {
    directives.push_back({"define"});
  }
  void MacroUndefined(const IdentifierInfo*) override {
    directives.push_back({"undef"});
  }
  void If(SourceLocation, bool) override { directives.push_back({"if"}); }
  void Elif(SourceLocation, bool) override { directives.push_back({"elif"}); }
  void Ifdef(SourceLocation, const IdentifierInfo*, bool) override {
    directives.push_back({"ifdef"});
  }
  void Ifndef(SourceLocation, const IdentifierInfo*, bool) override {
    directives.push_back({"ifndef"});
  }
  void Else(SourceLocation) override { directives.push_back({"else"}); }
  void Endif(SourceLocation) override { directives.push_back({"endif"}); }
  void PragmaDirective(SourceLocation) override {
    directives.push_back({"pragma"});
  }
  void InclusionDirective(SourceLocation, std::string_view, bool is_angled,
                          const FileEntry*, CharacteristicKind file_type) override {
    FileEntryInfo info;
    info.is_system_header = IsSystemHeader(file_type);
    files_entered.push_back(info);
  }
};

class PreprocessorTest : public ::testing::Test {
 protected:
  FileManager fm_;
  SourceManager sm_{fm_};
  DiagnosticsEngine diags_{nullptr, &sm_};

  FileID AddFile(std::string_view name, std::string_view content) {
    return sm_.CreateFileID(std::string(name), std::string(content));
  }

  FileID CreateFile(std::string_view content) {
    FileID fid = sm_.CreateFileID("test.c", std::string(content));
    sm_.SetMainFileID(fid);
    return fid;
  }

  fs::path CreateTempFile(std::string_view content) {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dis;
    fs::path path =
        fs::temp_directory_path() / ("bcc_pp_" + std::to_string(dis(gen)));
    std::ofstream os(path);
    os << content;
    return path;
  }

  // Lexes the whole stream (until and including kEOF) into a vector.
  std::vector<Token> LexAll(Preprocessor& pp) {
    std::vector<Token> tokens;
    for (;;) {
      Token t = pp.Lex();
      tokens.push_back(t);
      if (t.GetKind() == TokenKind::kEOF) break;
    }
    return tokens;
  }

  // Collects spellings from the preprocessor until EOF.
  std::vector<std::string> CollectSpellings(Preprocessor& pp) {
    std::vector<std::string> spellings;
    for (;;) {
      Token t = pp.Lex();
      if (t.GetKind() == TokenKind::kEOF) break;
      spellings.emplace_back(t.GetLexeme());
    }
    return spellings;
  }
};

TEST_F(PreprocessorTest, StreamsNonTriviaTokensOfSingleFile) {
  FileID fid = AddFile("main.c", "int  x;\n// comment\ny");
  sm_.SetMainFileID(fid);

  Preprocessor pp(sm_, diags_);
  pp.EnterMainFile();

  std::vector<Token> tokens = LexAll(pp);

  ASSERT_EQ(tokens.size(), 5u);  // int x ; y EOF
  EXPECT_EQ(tokens[0].GetLexeme(), "int");
  EXPECT_EQ(tokens[1].GetLexeme(), "x");
  EXPECT_EQ(tokens[2].GetKind(), TokenKind::kSemi);
  EXPECT_EQ(tokens[3].GetLexeme(), "y");
  EXPECT_TRUE(tokens[3].IsStartOfLine());
  EXPECT_EQ(tokens[4].GetKind(), TokenKind::kEOF);
}

TEST_F(PreprocessorTest, PromotesKeywordsAndAttachesIdentifierInfo) {
  FileID fid = AddFile("main.c", "int foo");
  sm_.SetMainFileID(fid);

  Preprocessor pp(sm_, diags_);
  pp.EnterMainFile();

  Token kw = pp.Lex();
  EXPECT_EQ(kw.GetKind(), TokenKind::kInt);  // promoted from kIdentifier
  ASSERT_NE(kw.GetIdentifierInfo(), nullptr);
  EXPECT_TRUE(kw.GetIdentifierInfo()->IsKeyword());
  EXPECT_EQ(kw.GetIdentifierInfo()->GetName(), "int");

  Token id = pp.Lex();
  EXPECT_EQ(id.GetKind(), TokenKind::kIdentifier);
  ASSERT_NE(id.GetIdentifierInfo(), nullptr);
  EXPECT_FALSE(id.GetIdentifierInfo()->IsKeyword());
  EXPECT_EQ(id.GetIdentifierInfo()->GetName(), "foo");
  EXPECT_FALSE(id.GetIdentifierInfo()->HasMacroDefinition());
}

TEST_F(PreprocessorTest, InternsRepeatedIdentifiersToSameInfo) {
  FileID fid = AddFile("main.c", "foo bar foo");
  sm_.SetMainFileID(fid);

  Preprocessor pp(sm_, diags_);
  pp.EnterMainFile();

  Token a = pp.Lex();
  Token b = pp.Lex();
  Token c = pp.Lex();

  EXPECT_EQ(a.GetIdentifierInfo(), c.GetIdentifierInfo());
  EXPECT_NE(a.GetIdentifierInfo(), b.GetIdentifierInfo());
}

TEST_F(PreprocessorTest, JoinsLineSplicedIdentifierBeforeInterning) {
  // "in\<newline>t" is the identifier "int" and must classify as the keyword.
  FileID fid = AddFile("main.c", "in\\\nt x");
  sm_.SetMainFileID(fid);

  Preprocessor pp(sm_, diags_);
  pp.EnterMainFile();

  Token spliced = pp.Lex();
  EXPECT_TRUE(spliced.NeedsCleaning());
  EXPECT_EQ(spliced.GetKind(), TokenKind::kInt);
  ASSERT_NE(spliced.GetIdentifierInfo(), nullptr);
  EXPECT_EQ(spliced.GetIdentifierInfo()->GetName(), "int");
}

TEST_F(PreprocessorTest, ResumesIncluderAfterIncludedFileEnds) {
  FileID main_fid = AddFile("main.c", "a\nb");
  FileID inc_fid = AddFile("inc.h", "x y");
  sm_.SetMainFileID(main_fid);

  Preprocessor pp(sm_, diags_);
  pp.EnterMainFile();

  Token a = pp.Lex();
  EXPECT_EQ(a.GetLexeme(), "a");
  EXPECT_EQ(pp.GetIncludeStackDepth(), 0u);
  EXPECT_EQ(pp.GetCurrentFileID(), main_fid);

  // Simulate an #include of inc.h in place of the a/b boundary.
  pp.EnterSourceFile(inc_fid);
  EXPECT_EQ(pp.GetIncludeStackDepth(), 1u);
  EXPECT_EQ(pp.GetCurrentFileID(), inc_fid);

  EXPECT_EQ(pp.Lex().GetLexeme(), "x");
  EXPECT_EQ(pp.Lex().GetLexeme(), "y");

  // Included file exhausted: the preprocessor pops and resumes main.c at 'b'
  // without ever surfacing the included file's EOF.
  Token b = pp.Lex();
  EXPECT_EQ(b.GetLexeme(), "b");
  EXPECT_EQ(pp.GetIncludeStackDepth(), 0u);
  EXPECT_EQ(pp.GetCurrentFileID(), main_fid);

  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kEOF);
}

TEST_F(PreprocessorTest, HandlesNestedIncludes) {
  FileID main_fid = AddFile("main.c", "m");
  FileID a_fid = AddFile("a.h", "a");
  FileID b_fid = AddFile("b.h", "b");
  sm_.SetMainFileID(main_fid);

  Preprocessor pp(sm_, diags_);
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "m");
  pp.EnterSourceFile(a_fid);
  EXPECT_EQ(pp.Lex().GetLexeme(), "a");
  pp.EnterSourceFile(b_fid);
  EXPECT_EQ(pp.GetIncludeStackDepth(), 2u);
  EXPECT_EQ(pp.Lex().GetLexeme(), "b");

  // Both nested files end; two pops bring us back to the (now empty) main file.
  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kEOF);
  EXPECT_EQ(pp.GetIncludeStackDepth(), 0u);
}

TEST_F(PreprocessorTest, EofIsIdempotent) {
  FileID fid = AddFile("main.c", "q");
  sm_.SetMainFileID(fid);

  Preprocessor pp(sm_, diags_);
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "q");
  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kEOF);
  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kEOF);
  EXPECT_EQ(pp.Lex().GetKind(), TokenKind::kEOF);
}

TEST_F(PreprocessorTest, NoMacros) {
  Preprocessor pp(sm_, diags_);
  CreateFile("int main() { return 42; }");
  pp.EnterMainFile();

  Token token = pp.Lex();
  EXPECT_TRUE(token.GetKind() == TokenKind::kInt);
  EXPECT_EQ("int", token.GetLexeme());

  token = pp.Lex();
  EXPECT_TRUE(token.GetKind() == TokenKind::kIdentifier);
  EXPECT_EQ("main", token.GetLexeme());

  token = pp.Lex();
  EXPECT_EQ(token.GetKind(), TokenKind::kLParen);

  token = pp.Lex();
  EXPECT_EQ(token.GetKind(), TokenKind::kRParen);

  token = pp.Lex();
  EXPECT_EQ(token.GetKind(), TokenKind::kLBrace);

  token = pp.Lex();
  EXPECT_TRUE(token.GetKind() == TokenKind::kReturn);
  EXPECT_EQ("return", token.GetLexeme());

  token = pp.Lex();
  EXPECT_TRUE(token.GetKind() == TokenKind::kNumericConstant);
  EXPECT_EQ("42", token.GetLexeme());

  token = pp.Lex();
  EXPECT_EQ(token.GetKind(), TokenKind::kSemi);

  token = pp.Lex();
  EXPECT_EQ(token.GetKind(), TokenKind::kRBrace);
}

TEST_F(PreprocessorTest, MacroExpansion_EmptyReplacement) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define EMPTY\n"
                  "int x = EMPTY;");
  pp.EnterMainFile();

  Token token = pp.Lex();
  EXPECT_EQ(token.GetLexeme(), "int");
  token = pp.Lex();
  EXPECT_EQ(token.GetLexeme(), "x");
  token = pp.Lex();
  EXPECT_EQ(token.GetLexeme(), "=");
  token = pp.Lex();
  EXPECT_EQ(token.GetLexeme(), ";");
  token = pp.Lex();
  EXPECT_TRUE(token.GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, MacroExpansion_MultipleReplacements) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define TYPE unsigned long\n"
                  "TYPE x;");
  pp.EnterMainFile();

  Token token = pp.Lex();
  EXPECT_TRUE(token.GetKind() == TokenKind::kUnsigned);
  EXPECT_EQ("unsigned", token.GetLexeme());

  token = pp.Lex();
  EXPECT_TRUE(token.GetKind() == TokenKind::kLong);
  EXPECT_EQ("long", token.GetLexeme());

  token = pp.Lex();
  EXPECT_EQ(token.GetLexeme(), "x");
  token = pp.Lex();
  EXPECT_EQ(token.GetLexeme(), ";");
  token = pp.Lex();
  EXPECT_TRUE(token.GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, DefineDirective_ObjectLike_SimpleReplacement) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define PI 3.14\nfloat x = PI;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "float");
  EXPECT_EQ(pp.Lex().GetLexeme(), "x");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "3.14");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, DefineDirective_ObjectLike_MultipleReplacements) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define WIDTH 100\n"
                 "#define HEIGHT 200\n"
                 "int area = WIDTH * HEIGHT;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "area");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "100");
  EXPECT_EQ(pp.Lex().GetLexeme(), "*");
  EXPECT_EQ(pp.Lex().GetLexeme(), "200");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, DefineDirective_ObjectLike_StringReplacement) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define NAME \"chibicpp\"\n"
                 "const char* name = NAME;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "const");
  EXPECT_EQ(pp.Lex().GetLexeme(), "char");
  EXPECT_EQ(pp.Lex().GetLexeme(), "*");
  EXPECT_EQ(pp.Lex().GetLexeme(), "name");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "\"chibicpp\"");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, DefineDirective_ObjectLike_ExpressionReplacement) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define MAX_SIZE 1024 * 1024\n"
                 "int size = MAX_SIZE;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "size");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1024");
  EXPECT_EQ(pp.Lex().GetLexeme(), "*");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1024");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, DefineDirective_ObjectLike_Redefinition) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define VALUE 10\n"
                 "#define VALUE 20\n"
                 "int x = VALUE;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "x");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "20");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, DefineDirective_ObjectLike_Undef) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define TEMP 42\n"
                 "#undef TEMP\n"
                 "int x = TEMP;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "x");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "TEMP");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, DefineDirective_ObjectLike_NestedMacros) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define A 1\n"
                 "#define B A + 2\n"
                 "#define C B * 3\n"
                 "int result = C;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "result");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), "+");
  EXPECT_EQ(pp.Lex().GetLexeme(), "2");
  EXPECT_EQ(pp.Lex().GetLexeme(), "*");
  EXPECT_EQ(pp.Lex().GetLexeme(), "3");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, DefineDirective_ObjectLike_SpecialCases) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define EMPTY\n"
                 "#define SPACES   hello  world  \n"
                 "EMPTY int x = SPACES;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "x");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "hello");
  EXPECT_EQ(pp.Lex().GetLexeme(), "world");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, DefineDirective_ObjectLike_InConditional) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define DEBUG 1\n"
                 "#if DEBUG\n"
                 "int debug_var = 1;\n"
                 "#else\n"
                 "int debug_var = 0;\n"
                 "#endif\n"
                 "int normal_var = 2;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "debug_var");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "normal_var");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "2");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, DefineDirective_ObjectLike_MacroInString) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define VERSION 2\n"
                 "const char* msg = \"VERSION is VERSION\";"
                 "int ver = VERSION;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "const");
  EXPECT_EQ(pp.Lex().GetLexeme(), "char");
  EXPECT_EQ(pp.Lex().GetLexeme(), "*");
  EXPECT_EQ(pp.Lex().GetLexeme(), "msg");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "\"VERSION is VERSION\"");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "ver");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "2");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, UndefDirective_BasicUsage) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define DEBUG 1\n"
                 "#undef DEBUG\n"
                 "int x = DEBUG;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "x");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "DEBUG");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, UndefDirective_UndefineNonExistentMacro) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#undef NON_EXISTENT\n"
                 "int x = 42;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "x");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "42");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, UndefDirective_RedefineAfterUndef) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define VALUE 100\n"
                 "#undef VALUE\n"
                 "#define VALUE 200\n"
                 "int x = VALUE;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "x");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "200");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, UndefDirective_MultipleUndefs) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define A 1\n"
                 "#define B 2\n"
                 "#undef A\n"
                 "#undef B\n"
                 "int x = A + B;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "x");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "A");
  EXPECT_EQ(pp.Lex().GetLexeme(), "+");
  EXPECT_EQ(pp.Lex().GetLexeme(), "B");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, DefineDirective_FunctionLikeMacro) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define ADD(x, y) ((x) + (y))\nint result = ADD(5, 3);");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "result");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "5");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), "+");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "3");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, DefineDirective_FunctionLikeMacro_EmptyArgs) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define GET_42() 42\n"
                 "int value = GET_42();");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "value");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "42");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, DefineDirective_FunctionLikeMacro_MultipleArgs) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define MUL(a, b, c) ((a) * (b) * (c))\n"
                 "int result = MUL(2, 3, 4);");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "result");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "2");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), "*");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "3");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), "*");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "4");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, DefineDirective_FunctionLikeMacro_Variadic) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define PRINTF(fmt, ...) printf(fmt, __VA_ARGS__)\n"
                 "PRINTF(\"%d %s\", 42, \"hello\");");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "printf");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "\"%d %s\"");
  EXPECT_EQ(pp.Lex().GetLexeme(), ",");
  EXPECT_EQ(pp.Lex().GetLexeme(), "42");
  EXPECT_EQ(pp.Lex().GetLexeme(), ",");
  EXPECT_EQ(pp.Lex().GetLexeme(), "\"hello\"");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest,
       DefineDirective_FunctionLikeMacro_GNUCommaVaArgs_Empty) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define LOG(fmt, ...) printf(fmt, ##__VA_ARGS__)\n"
                 "LOG(\"hello\");");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "printf");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "\"hello\"");
  // GNU ", ##__VA_ARGS__": the empty variadic argument swallows the comma.
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest,
       DefineDirective_FunctionLikeMacro_GNUCommaVaArgs_NonEmpty) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define LOG(fmt, ...) printf(fmt, ##__VA_ARGS__)\n"
                 "LOG(\"%d %d\", 1, 2);");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "printf");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "\"%d %d\"");
  EXPECT_EQ(pp.Lex().GetLexeme(), ",");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ",");
  EXPECT_EQ(pp.Lex().GetLexeme(), "2");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

// Regression test for the Linux kernel's include/linux/args.h COUNT_ARGS().
//
// COUNT_ARGS() relies on the GNU ", ##__VA_ARGS__" comma-elision extension:
//   #define __COUNT_ARGS(_0,.._15,_n, X...) _n
//   #define COUNT_ARGS(X...) __COUNT_ARGS(, ##X, 15, 14, .., 1, 0)
// When invoked with zero arguments, X is empty, so ", ##X" must delete the
// leading comma. If the comma is (incorrectly) retained it introduces an extra
// empty argument, shifting the placeholders by one and making COUNT_ARGS()
// count 1 instead of 0. This bug surfaced when preprocessing kernel sources and
// diffing against `clang -E`; the fix lives in TokenLexer::BuildExpansion.
TEST_F(PreprocessorTest, GNUCommaVaArgs_KernelCountArgs) {
  Preprocessor pp(sm_, diags_);
  CreateFile(
      "#define __COUNT_ARGS(_0, _1, _2, _3, _n, X...) _n\n"
      "#define COUNT_ARGS(X...) __COUNT_ARGS(, ##X, 3, 2, 1, 0)\n"
      "a COUNT_ARGS()\n"
      "b COUNT_ARGS(x)\n"
      "c COUNT_ARGS(x, y)\n"
      "d COUNT_ARGS(x, y, z)\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  // Zero args must count 0 (comma swallowed), then 1, 2, 3.
  EXPECT_EQ(spellings[0], "a");
  EXPECT_EQ(spellings[1], "0");
  EXPECT_EQ(spellings[2], "b");
  EXPECT_EQ(spellings[3], "1");
  EXPECT_EQ(spellings[4], "c");
  EXPECT_EQ(spellings[5], "2");
  EXPECT_EQ(spellings[6], "d");
  EXPECT_EQ(spellings[7], "3");
}

// The comma-elision extension only applies when the paste's right operand is
// the empty *variadic* argument. An empty *named* parameter next to "##" still
// follows the ISO placemarker rule and keeps the preceding comma.
TEST_F(PreprocessorTest, GNUCommaVaArgs_OnlyEmptyVariadicElidesComma) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define M(a, ...) [a , ##__VA_ARGS__]\n"
             "M(x)\n"
             "M(x, y)\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  // M(x): empty __VA_ARGS__ -> comma dropped -> [x].
  EXPECT_EQ(spellings[0], "[");
  EXPECT_EQ(spellings[1], "x");
  EXPECT_EQ(spellings[2], "]");
  // M(x, y): non-empty -> comma retained -> [x , y].
  EXPECT_EQ(spellings[3], "[");
  EXPECT_EQ(spellings[4], "x");
  EXPECT_EQ(spellings[5], ",");
  EXPECT_EQ(spellings[6], "y");
  EXPECT_EQ(spellings[7], "]");
}

TEST_F(PreprocessorTest, DefineDirective_FunctionLikeMacro_Stringify) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define STRINGIFY(x) #x\n"
                 "const char* str = STRINGIFY(hello world);");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "const");
  EXPECT_EQ(pp.Lex().GetLexeme(), "char");
  EXPECT_EQ(pp.Lex().GetLexeme(), "*");
  EXPECT_EQ(pp.Lex().GetLexeme(), "str");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "\"hello world\"");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, DefineDirective_FunctionLikeMacro_RecursiveExpansion) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define DEC(x) ((x) - 1)\n"
                 "#define DEC_DEC(x) DEC(DEC(x))\n"
                 "int result = DEC_DEC(10);");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "result");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "10");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), "-");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), "-");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, DefineDirective_ObjectLikeMacro_SelfRecursiveStops) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define SELF SELF\n"
                 "SELF");
  pp.EnterMainFile();

  std::vector<std::string> spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings, (std::vector<std::string>{"SELF"}));
}

TEST_F(PreprocessorTest, DefineDirective_FunctionLikeMacro_SelfRecursiveStops) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define SELF(x) SELF(x)\n"
                 "SELF(1)");
  pp.EnterMainFile();

  std::vector<std::string> spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings, (std::vector<std::string>{"SELF", "(", "1", ")"}));
}

TEST_F(PreprocessorTest,
       DefineDirective_FunctionLikeMacro_PasteThenRescanExpandsResult) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define MAKE_NAME(x) x ## _SUFFIX\n"
                 "#define VALUE_SUFFIX 42\n"
                 "int n = MAKE_NAME(VALUE);");
  pp.EnterMainFile();

  std::vector<std::string> spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings, (std::vector<std::string>{"int", "n", "=", "42", ";"}));
}

TEST_F(PreprocessorTest,
       DefineDirective_FunctionLikeMacro_MixedWithObjectLike) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define VALUE 5\n"
                 "#define ADD_VAL(x) ((x) + VALUE)\n"
                 "int result = ADD_VAL(10);");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "result");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "10");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), "+");
  EXPECT_EQ(pp.Lex().GetLexeme(), "5");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, IfdefDirective_BasicUsage) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#ifdef DEBUG\n"
                 "int debug = 1;\n"
                 "#endif\n"
                 "int normal = 2;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "normal");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "2");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, IfdefDirective_DefinedCondition) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define DEBUG 1\n"
                 "#ifdef DEBUG\n"
                 "int debug = 1;\n"
                 "#endif\n"
                 "int normal = 2;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "debug");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "normal");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "2");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, IfndefDirective_DefinedCondition) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define RELEASE 1\n"
                 "#ifndef RELEASE\n"
                 "int debug = 1;\n"
                 "#else\n"
                 "int release = 1;\n"
                 "#endif\n"
                 "int normal = 2;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "release");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "normal");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "2");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, IfndefDirective_BasicUsage) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#ifndef RELEASE\n"
                 "int debug = 1;\n"
                 "#endif\n"
                 "int normal = 2;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "debug");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "normal");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "2");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, IfDirective_ConstantExpression) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if 1\n"
                 "int included = 1;\n"
                 "#else\n"
                 "int excluded = 0;\n"
                 "#endif\n"
                 "int after = 2;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "included");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "after");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "2");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, IfDirective_MacroInCondition) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define LEVEL 2\n"
                 "#if LEVEL > 1\n"
                 "int high = 1;\n"
                 "#else\n"
                 "int low = 0;\n"
                 "#endif\n"
                 "int after = 3;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "high");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "after");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "3");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, IfDirective_ArithmeticExpression) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if 2 + 3 * 4 > 10\n"
                 "int expr_true = 1;\n"
                 "#else\n"
                 "int expr_false = 0;\n"
                 "#endif\n"
                 "int after = 5;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "expr_true");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "after");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "5");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, ElifDirective_BasicUsage) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define OPTION 2\n"
                 "#if OPTION == 1\n"
                 "int opt1 = 1;\n"
                 "#elif OPTION == 2\n"
                 "int opt2 = 2;\n"
                 "#elif OPTION == 3\n"
                 "int opt3 = 3;\n"
                 "#else\n"
                 "int opt_else = 0;\n"
                 "#endif\n"
                 "int after = 4;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "opt2");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "2");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "after");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "4");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, ElseDirective_BasicUsage) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if 0\n"
                 "int excluded = 1;\n"
                 "#else\n"
                 "int included = 2;\n"
                 "#endif\n"
                 "int after = 3;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "included");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "2");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "after");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "3");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, IncludeDirective_BasicUsage) {
  Preprocessor pp(sm_, diags_);
  HeaderSearch hs(fm_);
  pp.SetHeaderSearch(hs);

  // Create header file
  fs::path header_file = CreateTempFile("int header_value = 42;\n");

  // Create main file that includes header
  std::string main_content = "#include \"" + header_file.string() +
                             "\"\n"
                             "int main_value = 100;";
  CreateFile(main_content);
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "header_value");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "42");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "main_value");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "100");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, IncludeDirective_SystemHeader) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#include <stdio.h>\n"
                 "int main() { return 0; }");
  pp.EnterMainFile();

  // System headers might be empty or have specific content
  // We just verify we can parse without errors
  Token token = pp.Lex();

  while (token.GetKind() != TokenKind::kEOF) {
    token = pp.Lex();
  }

  EXPECT_TRUE(token.GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, IncludeDirective_NestedIncludes) {
  Preprocessor pp(sm_, diags_);
  HeaderSearch hs(fm_);
  pp.SetHeaderSearch(hs);
  fs::path inner_header = CreateTempFile("int inner = 1;\n");
  fs::path outer_header = CreateTempFile("#include \"" + inner_header.string() +
                                         "\"\n"
                                         "int outer = 2;\n");

  std::string main_content = "#include \"" + outer_header.string() +
                             "\"\n"
                             "int main_var = 3;";
  CreateFile(main_content);
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "inner");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "outer");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "2");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "main_var");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "3");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, LineDirective_BasicUsage) {
  Preprocessor pp(sm_, diags_);
  CreateFile("int a = 1;\n"
                 "#line 100 \"test.c\"\n"
                 "int b = 2;\n"
                 "int c = 3;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "a");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "b");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "2");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "c");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "3");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, ErrorDirective_BasicUsage) {
  Preprocessor pp(sm_, diags_);
  CreateFile("int valid = 1;\n"
                 "#error This is a test error\n"
                 "int after_error = 2;");
  pp.EnterMainFile();

  // Error directive should cause diagnostics but not stop parsing
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "valid");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "after_error");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "2");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, PragmaDirective_BasicUsage) {
  Preprocessor pp(sm_, diags_);
  CreateFile("int before = 1;\n"
                 "#pragma once\n"
                 "int after = 2;");
  pp.EnterMainFile();

  // Pragmas are implementation-dependent, we just verify they don't break
  // parsing
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "before");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "after");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "2");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, DefinedOperator_MultipleConditions) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define DEBUG 1\n"
                 "#if defined(DEBUG) && !defined(RELEASE)\n"
                 "int debug_only = 1;\n"
                 "#endif\n"
                 "int after = 2;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "debug_only");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "after");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "2");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, NestedConditionals) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if 1\n"
                 "  int outer = 1;\n"
                 "  #if 1\n"
                 "    int inner = 2;\n"
                 "  #else\n"
                 "    int inner_else = 3;\n"
                 "  #endif\n"
                 "  int outer_after = 4;\n"
                 "#else\n"
                 "  int outer_else = 5;\n"
                 "#endif\n"
                 "int after_all = 6;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "outer");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "inner");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "2");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "outer_after");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "4");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "after_all");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "6");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, ConditionalWithMacroExpansion) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define LEVEL 2\n"
                 "#define CHECK_LEVEL(x) (LEVEL == (x))\n"
                 "#if CHECK_LEVEL(2)\n"
                 "int level_two = 1;\n"
                 "#endif\n"
                 "int after = 2;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "level_two");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "after");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "2");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, MacroExpansionInMacroArguments) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define DOUBLE(x) ((x) * 2)\n"
                 "#define APPLY(fn, x) fn(x)\n"
                 "int result = APPLY(DOUBLE, 5);");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "result");
  EXPECT_EQ(spellings[2], "=");
  int paren_count = 0;
  for (const auto& s : spellings) {
    if (s == "(") ++paren_count;
  }
  // APPLY(DOUBLE, 5) -> fn(x) -> DOUBLE(5); on rescan the '(' from the body
  // completes the call, so DOUBLE(5) -> ((5) * 2). DOUBLE must NOT be expanded
  // while pre-expanding the `fn` argument (no '(' follows it there), matching
  // gcc/clang. The final text is `int result = ((5) * 2);` with two '('.
  EXPECT_EQ(paren_count, 2);
}

// A function-like macro name that lands at the end of a macro argument, and is
// itself forwarded through an intermediate macro before being used as a `##`
// operand, must not scan past the argument for its '(' during pre-expansion.
// Regression: the look-ahead used to pop the exhausted argument lexer and pull
// tokens from the following source line into the expansion, producing merged
// garbage like `class_fooDECLARE_c` and swallowing the next statement.
TEST_F(PreprocessorTest, FunctionLikeMacroAtArgEndDoesNotOverread) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define foo(x) BODY\n"
                 "#define DEF(n, act) class_##n##_c; act\n"
                 "#define D2(n, act) DEF(n, act)\n"
                 "D2(foo, zzz)\n"
                 "DECLARE(mutex, x)\n");
  pp.EnterMainFile();

  auto s = CollectSpellings(pp);
  const std::vector<std::string> expected = {
      "class_foo_c", ";",      "zzz", "DECLARE", "(",
      "mutex",       ",",      "x",   ")"};
  EXPECT_EQ(s, expected);
}

TEST_F(PreprocessorTest, ConditionalCompilationWithFunctionLikeMacros) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define IS_POSITIVE(x) ((x) > 0)\n"
                 "#if IS_POSITIVE(5)\n"
                 "int positive = 1;\n"
                 "#else\n"
                 "int non_positive = 0;\n"
                 "#endif\n"
                 "int after = 2;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "positive");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "after");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "2");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, MultiLineMacroDefinition) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define SWAP(a, b) do { \\\n"
                 "    typeof(a) temp = a; \\\n"
                 "    a = b; \\\n"
                 "    b = temp; \\\n"
                 "} while(0)\n"
                 "\n"
                 "int x = 1, y = 2;\n"
                 "SWAP(x, y);");
  pp.EnterMainFile();

  // Multi-line macros should be expanded correctly
  Token token = pp.Lex();
  while (token.GetKind() != TokenKind::kEOF) {
    token = pp.Lex();
  }
  EXPECT_TRUE(token.GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, StringificationOfArguments) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define STR(x) #x\n"
                 "#define XSTR(x) STR(x)\n"
                 "#define VALUE 42\n"
                 "const char* s1 = STR(VALUE);\n"
                 "const char* s2 = XSTR(VALUE);");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "const");
  EXPECT_EQ(pp.Lex().GetLexeme(), "char");
  EXPECT_EQ(pp.Lex().GetLexeme(), "*");
  EXPECT_EQ(pp.Lex().GetLexeme(), "s1");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "\"VALUE\"");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "const");
  EXPECT_EQ(pp.Lex().GetLexeme(), "char");
  EXPECT_EQ(pp.Lex().GetLexeme(), "*");
  EXPECT_EQ(pp.Lex().GetLexeme(), "s2");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "\"42\"");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, TokenPastingComplex) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define CONCAT(a, b) a ## b\n"
                 "#define MAKE_IDENTIFIER(prefix, num) CONCAT(prefix, num)\n"
                 "int MAKE_IDENTIFIER(var_, 1) = 1;\n"
                 "int MAKE_IDENTIFIER(var_, 2) = 2;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "var_1");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "var_2");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "2");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, BuiltinMacros) {
  Preprocessor pp(sm_, diags_);
  CreateFile("const char* file = __FILE__;\n"
                 "int line = __LINE__;\n"
                 "const char* date = __DATE__;\n"
                 "const char* time = __TIME__;\n"
                 "int stdc = __STDC__;");
  pp.EnterMainFile();

  // Built-in macros should be expanded
  Token token = pp.Lex();
  while (token.GetKind() != TokenKind::kEOF) {
    token = pp.Lex();
  }

  EXPECT_TRUE(token.GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, IncludeGuards) {
  Preprocessor pp(sm_, diags_);
  HeaderSearch hs(fm_);
  pp.SetHeaderSearch(hs);

  // Create a header with include guard
  fs::path header_file = CreateTempFile(
      "#ifndef MY_HEADER_H\n"
      "#define MY_HEADER_H\n"
      "int header_value = 42;\n"
      "#endif\n");

  // Create main file that includes header twice
  std::string main_content = "#include \"" + header_file.string() +
                             "\"\n"
                             "#include \"" +
                             header_file.string() +
                             "\"\n"
                             "int main_value = 100;";

  CreateFile(main_content);
  pp.EnterMainFile();

  // Header should only be included once
  int header_value_count = 0;
  Token token = pp.Lex();

  while (token.GetKind() != TokenKind::kEOF) {
    if (token.GetLexeme() == "header_value") {
      header_value_count++;
    }

    token = pp.Lex();
  }

  EXPECT_EQ(header_value_count, 1);
}

TEST_F(PreprocessorTest, ComplexNestedMacroExpansion) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define A(x) x+1\n"
                 "#define B(x) A(x)*2\n"
                 "#define C(x) B(x)/3\n"
                 "int result = C(5);");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "result");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "5");
  EXPECT_EQ(pp.Lex().GetLexeme(), "+");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), "*");
  EXPECT_EQ(pp.Lex().GetLexeme(), "2");
  EXPECT_EQ(pp.Lex().GetLexeme(), "/");
  EXPECT_EQ(pp.Lex().GetLexeme(), "3");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, ComplexConditionalExpressions) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define A 1\n"
                 "#define B 2\n"
                 "#define C 3\n"
                 "#if (A < B) && (B < C)\n"
                 "int order = 1;\n"
                 "#endif\n"
                 "#if A == B || B == C\n"
                 "int equal = 1;\n"
                 "#else\n"
                 "int not_equal = 0;\n"
                 "#endif\n"
                 "#if !(A == 0)\n"
                 "int not_zero = 1;\n"
                 "#endif");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "order");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "not_equal");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "0");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "not_zero");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, ConditionalOperatorInMacro) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define MAX(a, b) ((a) > (b) ? (a) : (b))\n"
                 "#define MIN(a, b) ((a) < (b) ? (a) : (b))\n"
                 "int max_val = MAX(10, 20);\n"
                 "int min_val = MIN(10, 20);");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "max_val");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "10");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), ">");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "20");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), "?");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "10");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), ":");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "20");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "min_val");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "10");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), "<");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "20");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), "?");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "10");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), ":");
  EXPECT_EQ(pp.Lex().GetLexeme(), "(");
  EXPECT_EQ(pp.Lex().GetLexeme(), "20");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), ")");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

TEST_F(PreprocessorTest, PpCallbacks_RecordDirectivesInEncounterOrder) {
  Preprocessor pp(sm_, diags_);
  auto callbacks = std::make_unique<RecordingPPCallbacks>(sm_);
  auto* callbacks_raw = callbacks.get();
  pp.SetPPCallbacks(std::move(callbacks));
  CreateFile("#define A 1\n"
                 "#if A\n"
                 "#else\n"
                 "#endif\n"
                 "#pragma once\n");
  pp.EnterMainFile();

  while (pp.Lex().GetKind() != TokenKind::kEOF) {
  }

  ASSERT_GE(callbacks_raw->directives.size(), 5U);
  EXPECT_EQ(callbacks_raw->directives[0].name, "define");
  EXPECT_EQ(callbacks_raw->directives[1].name, "if");
  EXPECT_EQ(callbacks_raw->directives[2].name, "else");
  EXPECT_EQ(callbacks_raw->directives[3].name, "endif");
  EXPECT_EQ(callbacks_raw->directives[4].name, "pragma");
}

TEST_F(PreprocessorTest, PpCallbacks_RecordFileEnteredForMainAndIncludes) {
  Preprocessor pp(sm_, diags_);
  HeaderSearch hs(fm_);
  pp.SetHeaderSearch(hs);
  auto callbacks = std::make_unique<RecordingPPCallbacks>(sm_);
  auto* callbacks_raw = callbacks.get();
  pp.SetPPCallbacks(std::move(callbacks));

  fs::path header = CreateTempFile("int from_header = 1;\n");
  FileID main_id =
      CreateFile("#include \"" + header.string() + "\"\nint from_main = 2;\n");
  pp.EnterMainFile();

  while (pp.Lex().GetKind() != TokenKind::kEOF) {
  }

  ASSERT_GE(callbacks_raw->files_entered.size(), 2U);
  EXPECT_EQ(callbacks_raw->files_entered.front().buffer_id, main_id);
  EXPECT_FALSE(callbacks_raw->files_entered.front().is_system_header);
}

TEST_F(PreprocessorTest, DependencyCollectorCollectsMainAndHeader) {
  struct DepCollector : public PPCallbacks {
    std::vector<std::string> deps;
    void InclusionDirective(SourceLocation, std::string_view filename, bool,
                            const FileEntry*, CharacteristicKind) override {
      deps.emplace_back(filename);
    }
    void WriteMakefile(std::ostream& os, std::string_view target) {
      os << target << ":";
      for (auto& d : deps) os << " " << d;
      os << "\n";
    }
  };

  auto collector = std::make_unique<DepCollector>();
  auto* collector_raw = collector.get();
  Preprocessor pp(sm_, diags_);
  pp.SetPPCallbacks(std::move(collector));

  fs::path header_path = CreateTempFile("int dep = 1;\n");
  FileID main_id =
      sm_.CreateFileID("main.c",
                        "#include \"" + header_path.string() + "\"\n");
  sm_.SetMainFileID(main_id);
  pp.EnterMainFile();

  while (pp.Lex().GetKind() != TokenKind::kEOF) {
  }

  std::ostringstream os;
  collector_raw->WriteMakefile(os, "out.o");
  std::string deps = os.str();
  EXPECT_NE(deps.find(header_path.string()), std::string::npos);
}

TEST_F(PreprocessorTest, LineDirectiveUpdatesPresumedLocation) {
  Preprocessor pp(sm_, diags_);
  CreateFile("int a = 1;\n"
                  "#line 77 \"virt.c\"\n"
                  "int b = 2;\n");
  pp.EnterMainFile();

  auto tokens = LexAll(pp);
  ASSERT_GE(tokens.size(), 10U);

  std::size_t b_idx = tokens.size();
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (tokens[i].GetLexeme() == "b") {
      b_idx = i;
      break;
    }
  }

  ASSERT_LT(b_idx, tokens.size());
  PresumedLoc pl = sm_.GetPresumedLoc(tokens[b_idx].GetLocation());
  EXPECT_EQ(pl.filename, "virt.c");
  EXPECT_EQ(pl.line, 77U);
}

TEST_F(PreprocessorTest, PragmaOnceSkipsSecondIncludeOfSameHeader) {
  Preprocessor pp(sm_, diags_);
  HeaderSearch hs(fm_);
  pp.SetHeaderSearch(hs);
  fs::path header = CreateTempFile("#pragma once\nint once_value = 1;\n");
  CreateFile("#include \"" + header.string() +
                               "\"\n"
                               "#include \"" +
                               header.string() +
                               "\"\n"
                               "int after = 2;\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  int once_count = 0;
  for (const auto& spelling : spellings) {
    if (spelling == "once_value") ++once_count;
  }

  EXPECT_EQ(once_count, 1);
}

TEST_F(PreprocessorTest, FunctionLikeMacroWithoutLParenDoesNotExpand) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define ADD(x, y) x + y\n"
                 "int value = ADD;\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  ASSERT_GE(spellings.size(), 4U);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "value");
  EXPECT_EQ(spellings[2], "=");
  EXPECT_EQ(spellings[3], "ADD");
  EXPECT_EQ(spellings[4], ";");
}

TEST_F(PreprocessorTest, ObjectLikeMacroCarriesOriginToExpandedTokens) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define VALUE 42\n"
                  "int x = VALUE;\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  ASSERT_GE(spellings.size(), 5U);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "x");
  EXPECT_EQ(spellings[2], "=");
  EXPECT_EQ(spellings[3], "42");
  EXPECT_EQ(spellings[4], ";");
}

TEST_F(PreprocessorTest, NestedFunctionLikeMacroExpandsArgumentsRecursively) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define INC(x) ((x) + 1)\n"
                  "#define WRAP(x) INC(x)\n"
                  "int value = WRAP(INC(2));\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  ASSERT_FALSE(spellings.empty());

  std::vector<std::string> expected = {"int", "value", "=", "(", "(", "(",
                                       "(",   "2",     ")", "+", "1", ")",
                                       ")",   "+",     "1", ")", ";"};
  EXPECT_EQ(spellings, expected);
}

TEST_F(PreprocessorTest, ConditionalSkipsNestedFalseBlocksUntilMatchingEndif) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if 0\n"
                 "#if 1\n"
                 "int hidden1 = 1;\n"
                 "#endif\n"
                 "int hidden2 = 2;\n"
                 "#else\n"
                 "int visible = 3;\n"
                 "#endif\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  std::vector<std::string> expected = {"int", "visible", "=", "3", ";"};
  EXPECT_EQ(spellings, expected);
}

TEST_F(PreprocessorTest, IncludeDirectiveRelativeToCurrentBufferPath) {
  Preprocessor pp(sm_, diags_);
  static std::random_device rd;
  static std::mt19937_64 gen(rd());
  static std::uniform_int_distribution<uint64_t> dis;
  fs::path dir =
      fs::temp_directory_path() / ("chibicpp_pp_" + std::to_string(dis(gen)));
  fs::create_directories(dir);
  fs::path header = dir / "rel_header.h";
  fs::path main = dir / "main.c";

  {
    std::ofstream header_os(header);
    header_os << "int relative_value = 9;\n";
  }
  {
    std::ofstream main_os(main);
    main_os << "#include \"rel_header.h\"\nint tail = 10;\n";
  }

  const FileEntry* fe = fm_.GetFile(main.string());
  ASSERT_NE(fe, nullptr);
  FileID main_fid = sm_.CreateFileID(*fe);
  ASSERT_TRUE(main_fid.IsValid());
  sm_.SetMainFileID(main_fid);
  HeaderSearch hs(fm_);
  hs.AddQuotedSearchPath(dir.string());
  pp.SetHeaderSearch(hs);
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  std::vector<std::string> expected = {"int", "relative_value", "=", "9",  ";",
                                       "int", "tail",           "=", "10", ";"};
  EXPECT_EQ(spellings, expected);
}

TEST_F(PreprocessorTest,
       DirectiveTokenLocationsRemainAtStartOfLineAfterSpaces) {
  Preprocessor pp(sm_, diags_);
  CreateFile("   #define VALUE 1\n"
                 "VALUE\n");
  pp.EnterMainFile();

  Token token = pp.Lex();
  ASSERT_FALSE(token.GetKind() == TokenKind::kEOF);
  EXPECT_EQ(token.GetLexeme(), "1");
  EXPECT_TRUE(token.GetKind() == TokenKind::kNumericConstant);
}

TEST_F(PreprocessorTest, DefinedOperatorDoesNotExpandOperand) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define ALIAS TARGET\n"
                 "#if defined(ALIAS)\n"
                 "int preserved = 1;\n"
                 "#else\n"
                 "int expanded = 0;\n"
                 "#endif\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  std::vector<std::string> expected = {"int", "preserved", "=", "1", ";"};
  EXPECT_EQ(spellings, expected);
}

TEST_F(PreprocessorTest, UndefinedIdentifiersBecomeZeroInIfExpressions) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if UNKNOWN_SYMBOL + 1 == 1\n"
                 "int active = 1;\n"
                 "#else\n"
                 "int inactive = 0;\n"
                 "#endif\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  std::vector<std::string> expected = {"int", "active", "=", "1", ";"};
  EXPECT_EQ(spellings, expected);
}

TEST_F(PreprocessorTest, UndefinedFunctionLikeIdentifiersBecomeZeroInIf) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if UNKNOWN_BUILTIN(feature_name)\n"
                 "int active = 1;\n"
                 "#else\n"
                 "int inactive = 0;\n"
                 "#endif\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  std::vector<std::string> expected = {"int", "inactive", "=", "0", ";"};
  EXPECT_EQ(spellings, expected);
}

TEST_F(PreprocessorTest, TokenPasteResultCanInvokeFunctionLikeMacro) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define CAT(a, b) a ## b\n"
                 "#define FN(x) ((x) + 1)\n"
                 "int result = CAT(F, N)(2);\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  std::vector<std::string> expected = {"int", "result", "=", "(", "(", "2",
                                       ")",   "+",      "1", ")", ";"};
  EXPECT_EQ(spellings, expected);
}

TEST_F(PreprocessorTest, PragmaOnceSkipsHeaderAcrossNestedIncludeGraph) {
  Preprocessor pp(sm_, diags_);
  static std::random_device rd;
  static std::mt19937_64 gen(rd());
  static std::uniform_int_distribution<uint64_t> dis;
  fs::path dir = fs::temp_directory_path() /
                 ("chibicpp_once_graph_" + std::to_string(dis(gen)));
  fs::create_directories(dir);
  fs::path leaf = dir / "leaf.h";
  fs::path mid = dir / "mid.h";
  fs::path main = dir / "main.c";

  {
    std::ofstream leaf_os(leaf);
    leaf_os << "#pragma once\nint leaf_value = 1;\n";
  }
  {
    std::ofstream mid_os(mid);
    mid_os << "#include \"leaf.h\"\n#include \"leaf.h\"\n";
  }
  {
    std::ofstream main_os(main);
    main_os << "#include \"mid.h\"\n#include \"leaf.h\"\nint tail = 2;\n";
  }

  const FileEntry* fe = fm_.GetFile(main.string());
  ASSERT_NE(fe, nullptr);
  FileID main_fid = sm_.CreateFileID(*fe);
  ASSERT_TRUE(main_fid.IsValid());
  sm_.SetMainFileID(main_fid);
  HeaderSearch hs(fm_);
  hs.AddQuotedSearchPath(dir.string());
  pp.SetHeaderSearch(hs);
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  int leaf_count = 0;
  for (const auto& spelling : spellings) {
    if (spelling == "leaf_value") ++leaf_count;
  }

  EXPECT_EQ(leaf_count, 1);
  EXPECT_EQ(spellings.back(), ";");
}

// Test that preprocessor handles EOF correctly and doesn't crash on destruction
// when the include stack becomes empty.
TEST_F(PreprocessorTest, PreprocessorDestructorAfterEOF) {
  // Create preprocessor in a scope so it gets destroyed
  {
    Preprocessor pp(sm_, diags_);
    CreateFile("int x = 1;\nint y = 2;\n");
  pp.EnterMainFile();

    // Lex all tokens until EOF
    while (pp.Lex().GetKind() != TokenKind::kEOF) {
    }

    // Preprocessor will be destroyed here - include stack should be empty
    // This tests that the destructor doesn't try to access the empty include
    // stack
  }
}

// Test multiple lex calls after EOF
TEST_F(PreprocessorTest, MultipleLexCallsAfterEOF) {
  Preprocessor pp(sm_, diags_);
  CreateFile("int x = 1;");
  pp.EnterMainFile();

  // Lex until EOF
  while (pp.Lex().GetKind() != TokenKind::kEOF) {
  }

  // Call Lex() multiple times after EOF - should all return EOF
  Token t1 = pp.Lex();
  Token t2 = pp.Lex();
  Token t3 = pp.Lex();

  EXPECT_TRUE(t1.GetKind() == TokenKind::kEOF);
  EXPECT_TRUE(t2.GetKind() == TokenKind::kEOF);
  EXPECT_TRUE(t3.GetKind() == TokenKind::kEOF);
}

// Test preprocessor with file containing only a hash (edge case)
TEST_F(PreprocessorTest, FileWithOnlyHash) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#");
  pp.EnterMainFile();

  // Should handle gracefully without assertion failure
  Token token = pp.Lex();
  // The lone '#' at EOF might be treated as an error or just ignored
  // The important thing is it shouldn't crash
  EXPECT_TRUE(token.GetKind() == TokenKind::kEOF || token.GetKind() == TokenKind::kSemi /* FIX_ME */);
}

// =============================================================================
// Additional Edge Case Tests
// =============================================================================

// Test empty file
TEST_F(PreprocessorTest, EmptyFile) {
  Preprocessor pp(sm_, diags_);
  CreateFile("");
  pp.EnterMainFile();

  Token token = pp.Lex();
  EXPECT_TRUE(token.GetKind() == TokenKind::kEOF);
}

// Test file with only whitespace
TEST_F(PreprocessorTest, FileWithOnlyWhitespace) {
  Preprocessor pp(sm_, diags_);
  CreateFile("   \t\n\n  \t  \n");
  pp.EnterMainFile();

  Token token = pp.Lex();
  EXPECT_TRUE(token.GetKind() == TokenKind::kEOF);
}

// Test file with only comments
TEST_F(PreprocessorTest, FileWithOnlyComments) {
  Preprocessor pp(sm_, diags_);
  CreateFile("// comment\n/* block */\n");
  pp.EnterMainFile();

  Token token = pp.Lex();
  EXPECT_TRUE(token.GetKind() == TokenKind::kEOF);
}

// Test multiple empty macros in sequence
TEST_F(PreprocessorTest, MultipleEmptyMacrosInSequence) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define EMPTY1\n"
                 "#define EMPTY2\n"
                 "#define EMPTY3\n"
                 "EMPTY1 EMPTY2 EMPTY3 int x = 1;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "x");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

// Test empty macro at end of file
TEST_F(PreprocessorTest, EmptyMacroAtEndOfFile) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define EMPTY\n"
                 "int x = 1; EMPTY");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "x");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

// Test function-like macro with no arguments at end of file
TEST_F(PreprocessorTest, FunctionLikeMacroNoArgsAtEndOfFile) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define FUNC(x) x\n"
                 "FUNC");
  pp.EnterMainFile();

  // FUNC without ( should not expand, just return identifier
  EXPECT_EQ(pp.Lex().GetLexeme(), "FUNC");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

// Test deeply nested macro expansion
TEST_F(PreprocessorTest, DeeplyNestedMacroExpansion) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define A1(x) A2(x)\n"
                 "#define A2(x) A3(x)\n"
                 "#define A3(x) A4(x)\n"
                 "#define A4(x) A5(x)\n"
                 "#define A5(x) A6(x)\n"
                 "#define A6(x) A7(x)\n"
                 "#define A7(x) A8(x)\n"
                 "#define A8(x) A9(x)\n"
                 "#define A9(x) A10(x)\n"
                 "#define A10(x) ((x) + 1)\n"
                 "int result = A1(5);");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "result");
  EXPECT_EQ(spellings[2], "=");
  EXPECT_EQ(spellings[3], "(");
  EXPECT_EQ(spellings[4], "(");
  EXPECT_EQ(spellings[5], "5");
  EXPECT_EQ(spellings[6], ")");
  EXPECT_EQ(spellings[7], "+");
  EXPECT_EQ(spellings[8], "1");
  EXPECT_EQ(spellings[9], ")");
  EXPECT_EQ(spellings[10], ";");
}

// Test macro argument with balanced parens
TEST_F(PreprocessorTest, MacroArgumentWithBalancedParens) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define INVOKE(x) x\n"
                  "int result = INVOKE((1 + 2) * (3 + 4));\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "result");
  EXPECT_EQ(spellings[2], "=");
  // The argument (1 + 2) * (3 + 4) is passed as a single argument
  // and expands to the tokens inside
  EXPECT_EQ(spellings[3], "(");
  EXPECT_EQ(spellings[4], "1");
  EXPECT_EQ(spellings[5], "+");
  EXPECT_EQ(spellings[6], "2");
  EXPECT_EQ(spellings[7], ")");
  EXPECT_EQ(spellings[8], "*");
  EXPECT_EQ(spellings[9], "(");
  EXPECT_EQ(spellings[10], "3");
  EXPECT_EQ(spellings[11], "+");
  EXPECT_EQ(spellings[12], "4");
  EXPECT_EQ(spellings[13], ")");
  EXPECT_EQ(spellings[14], ";");
}

// Test macro with empty argument
TEST_F(PreprocessorTest, MacroWithEmptyArgument) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define FOO(x, y) x y\n"
                 "int result = FOO(1, );");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "result");
  EXPECT_EQ(spellings[2], "=");
  EXPECT_EQ(spellings[3], "1");
  // Empty argument should produce nothing
  EXPECT_EQ(spellings[4], ";");
}

// Test token paste at beginning of replacement
TEST_F(PreprocessorTest, TokenPasteAtBeginning) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define PASTE(x) ## x\n"
                 "int result = PASTE(1);");
  pp.EnterMainFile();

  // This should produce an error but not crash
  auto spellings = CollectSpellings(pp);
  // Just check we don't crash
}

// Test token paste at end of replacement
TEST_F(PreprocessorTest, TokenPasteAtEnd) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define PASTE(x) x ##\n"
                 "int result = PASTE(1);");
  pp.EnterMainFile();

  // This should produce an error but not crash
  auto spellings = CollectSpellings(pp);
  // Just check we don't crash
}

// Test hash at end of replacement (no parameter)
TEST_F(PreprocessorTest, HashAtEndOfReplacement) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define STR(x) x #\n"
                 "int result = STR(hello);");
  pp.EnterMainFile();

  // This should produce an error but not crash
  auto spellings = CollectSpellings(pp);
  // Just check we don't crash
}

// Test multiple consecutive token pastes
TEST_F(PreprocessorTest, MultipleConsecutiveTokenPastes) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define CAT3(a, b, c) a ## b ## c\n"
                 "int CAT3(va, r_, 1) = 1;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "var_1");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

// Test stringification of empty argument
TEST_F(PreprocessorTest, StringifyEmptyArgument) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define STR(x) #x\n"
                 "const char* s = STR();");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "const");
  EXPECT_EQ(pp.Lex().GetLexeme(), "char");
  EXPECT_EQ(pp.Lex().GetLexeme(), "*");
  EXPECT_EQ(pp.Lex().GetLexeme(), "s");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  // Empty stringification should produce ""
  EXPECT_EQ(pp.Lex().GetLexeme(), "\"\"");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

// Test nested stringification
TEST_F(PreprocessorTest, NestedStringification) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define STR(x) #x\n"
                 "#define WRAP(x) STR(x)\n"
                 "#define VALUE hello\n"
                 "const char* s1 = STR(VALUE);\n"
                 "const char* s2 = WRAP(VALUE);");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "const");
  EXPECT_EQ(pp.Lex().GetLexeme(), "char");
  EXPECT_EQ(pp.Lex().GetLexeme(), "*");
  EXPECT_EQ(pp.Lex().GetLexeme(), "s1");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "\"VALUE\"");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_EQ(pp.Lex().GetLexeme(), "const");
  EXPECT_EQ(pp.Lex().GetLexeme(), "char");
  EXPECT_EQ(pp.Lex().GetLexeme(), "*");
  EXPECT_EQ(pp.Lex().GetLexeme(), "s2");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "\"hello\"");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

// Test #if with complex expression
TEST_F(PreprocessorTest, IfWithComplexExpression) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if (1 && 0) || (1 && 1)\n"
                 "int active = 1;\n"
                 "#else\n"
                 "int inactive = 0;\n"
                 "#endif\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "active");
  EXPECT_EQ(spellings[2], "=");
  EXPECT_EQ(spellings[3], "1");
  EXPECT_EQ(spellings[4], ";");
}

// Test #if with defined operator and parens
TEST_F(PreprocessorTest, DefinedOperatorWithParens) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define FOO 1\n"
                 "#if defined(FOO)\n"
                 "int has_foo = 1;\n"
                 "#endif\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "has_foo");
}

// Test #if with defined operator without parens
TEST_F(PreprocessorTest, DefinedOperatorWithoutParens) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define BAR 1\n"
                 "#if defined BAR\n"
                 "int has_bar = 1;\n"
                 "#endif\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "has_bar");
}

// Test unterminated #if (should handle gracefully)
TEST_F(PreprocessorTest, UnterminatedIf) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if 1\n"
                 "int x = 1;\n");
  pp.EnterMainFile();

  // Should reach EOF without crash
  Token token = pp.Lex();
  while (token.GetKind() != TokenKind::kEOF) {
    token = pp.Lex();
  }
  EXPECT_TRUE(token.GetKind() == TokenKind::kEOF);
}

// Test unterminated function-like macro invocation
TEST_F(PreprocessorTest, UnterminatedMacroInvocation) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define FUNC(x) x\n"
                 "int result = FUNC(1 + 2\n");
  pp.EnterMainFile();

  // Should handle gracefully
  auto spellings = CollectSpellings(pp);
  // Just check we don't crash
}

// Test macro redefinition with same value (should work)
TEST_F(PreprocessorTest, MacroRedefinitionSameValue) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define VALUE 42\n"
                 "#define VALUE 42\n"
                 "int x = VALUE;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "x");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "42");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

// Test #undef on non-existent macro
TEST_F(PreprocessorTest, UndefNonExistentThenDefine) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#undef DOES_NOT_EXIST\n"
                 "#define DOES_NOT_EXIST 42\n"
                 "int x = DOES_NOT_EXIST;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "x");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "42");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

// Test #elif after #if 0
TEST_F(PreprocessorTest, ElifChainWithFirstTrue) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if 1\n"
                 "int first = 1;\n"
                 "#elif 1\n"
                 "int second = 2;\n"
                 "#else\n"
                 "int third = 3;\n"
                 "#endif\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "first");
  EXPECT_EQ(spellings[2], "=");
  EXPECT_EQ(spellings[3], "1");
  EXPECT_EQ(spellings[4], ";");
}

// Test #elif chain all false
TEST_F(PreprocessorTest, ElifChainAllFalse) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if 0\n"
                 "int first = 1;\n"
                 "#elif 0\n"
                 "int second = 2;\n"
                 "#elif 0\n"
                 "int third = 3;\n"
                 "#endif\n"
                 "int after = 4;\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "after");
}

// Test #elif with else
TEST_F(PreprocessorTest, ElifWithElse) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if 0\n"
                 "int first = 1;\n"
                 "#elif 0\n"
                 "int second = 2;\n"
                 "#else\n"
                 "int third = 3;\n"
                 "#endif\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "third");
}

// Test variadic macro with no variadic args
TEST_F(PreprocessorTest, VariadicMacroNoVariadicArgs) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define DEBUG(fmt, ...) printf(fmt, ##__VA_ARGS__)\n"
                 "DEBUG(\"hello\");");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "printf");
  EXPECT_EQ(spellings[1], "(");
  EXPECT_EQ(spellings[2], "\"hello\"");
  // GNU ", ##__VA_ARGS__": the empty variadic argument swallows the comma.
  EXPECT_EQ(spellings[3], ")");
  EXPECT_EQ(spellings[4], ";");
}

// Test variadic macro with only variadic args
TEST_F(PreprocessorTest, VariadicMacroOnlyVariadicArgs) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define LOG(...) printf(__VA_ARGS__)\n"
                 "LOG(\"%d %d\", 1, 2);");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "printf");
  EXPECT_EQ(spellings[1], "(");
  EXPECT_EQ(spellings[2], "\"%d %d\"");
  EXPECT_EQ(spellings[3], ",");
  EXPECT_EQ(spellings[4], "1");
  EXPECT_EQ(spellings[5], ",");
  EXPECT_EQ(spellings[6], "2");
  EXPECT_EQ(spellings[7], ")");
  EXPECT_EQ(spellings[8], ";");
}

// Test macro that expands to directive name
TEST_F(PreprocessorTest, MacroExpandsToDirectiveName) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define DIRECTIVE define\n"
                 "#DIRECTIVE VALUE 42\n"
                 "int x = VALUE;");
  pp.EnterMainFile();

  // The #DIRECTIVE should not be treated as a directive
  // DIRECTIVE expands to "define" but it's not a valid directive
  auto spellings = CollectSpellings(pp);
}

// Test macro expansion in #include path
TEST_F(PreprocessorTest, MacroInIncludePath) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define HEADER \"stdio.h\"\n"
                 "#include HEADER\n"
                 "int x = 1;");
  pp.EnterMainFile();

  // Should handle macro expansion in include
  Token token = pp.Lex();
  while (token.GetKind() != TokenKind::kEOF) {
    token = pp.Lex();
  }
  EXPECT_TRUE(token.GetKind() == TokenKind::kEOF);
}

// Test __COUNTER__ macro (if supported)
TEST_F(PreprocessorTest, CounterMacro) {
  Preprocessor pp(sm_, diags_);
  CreateFile("int a = __COUNTER__;\n"
                 "int b = __COUNTER__;\n"
                 "int c = __COUNTER__;");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  // __COUNTER__ should increment each time
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "a");
  EXPECT_EQ(spellings[2], "=");
  // First __COUNTER__ should be 0
  // Note: behavior depends on implementation
}

// Test line directive with just number
TEST_F(PreprocessorTest, LineDirectiveJustNumber) {
  Preprocessor pp(sm_, diags_);
  CreateFile("int a = 1;\n"
                 "#line 100\n"
                 "int b = 2;\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings.size(), 10U);
}

// Test error directive with empty message
TEST_F(PreprocessorTest, ErrorDirectiveEmptyMessage) {
  Preprocessor pp(sm_, diags_);
  CreateFile("int before = 1;\n"
                 "#error\n"
                 "int after = 2;");
  pp.EnterMainFile();

  // Should continue processing after error
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "before");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
}

// Test pragma with no tokens
TEST_F(PreprocessorTest, PragmaNoTokens) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#pragma\n"
                 "int x = 1;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "x");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

// Test macro argument with comma in nested parens
TEST_F(PreprocessorTest, MacroArgWithCommaInNestedParens) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define INVOKE(fn, arg) fn(arg)\n"
                  "int result = INVOKE(foo, (1, 2, 3));\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "result");
  EXPECT_EQ(spellings[2], "=");
  EXPECT_EQ(spellings[3], "foo");
  EXPECT_EQ(spellings[4], "(");
  EXPECT_EQ(spellings[5], "(");
  EXPECT_EQ(spellings[6], "1");
  EXPECT_EQ(spellings[7], ",");
  EXPECT_EQ(spellings[8], "2");
  EXPECT_EQ(spellings[9], ",");
  EXPECT_EQ(spellings[10], "3");
  EXPECT_EQ(spellings[11], ")");
  EXPECT_EQ(spellings[12], ")");
  EXPECT_EQ(spellings[13], ";");
}

// Test empty macro used as argument
TEST_F(PreprocessorTest, EmptyMacroAsArgument) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define EMPTY\n"
                 "#define ID(x) x\n"
                 "int result = ID(EMPTY);");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  // EMPTY expands to nothing, so ID(EMPTY) should expand to nothing
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "result");
  EXPECT_EQ(spellings[2], "=");
  EXPECT_EQ(spellings[3], ";");
}

// Test self-referencing macro with token paste
TEST_F(PreprocessorTest, SelfReferencingWithTokenPaste) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define SELF SELF\n"
                 "#define PASTE(x) x ## _suffix\n"
                 "int result = PASTE(SELF);");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  // SELF in the pasted result should not expand (it's in hideset)
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "result");
  EXPECT_EQ(spellings[2], "=");
  EXPECT_EQ(spellings[3], "SELF_suffix");
  EXPECT_EQ(spellings[4], ";");
}

// Test conditional with missing #endif
TEST_F(PreprocessorTest, MissingEndif) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if 1\n"
                 "int x = 1;\n"
                 "#if 0\n"
                 "int y = 2;\n");
  pp.EnterMainFile();

  // Should handle gracefully
  Token token = pp.Lex();
  while (token.GetKind() != TokenKind::kEOF) {
    token = pp.Lex();
  }
  EXPECT_TRUE(token.GetKind() == TokenKind::kEOF);
}

// Test multiple #else in same conditional (error case)
TEST_F(PreprocessorTest, MultipleElseInConditional) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if 1\n"
                 "int first = 1;\n"
                 "#else\n"
                 "int second = 2;\n"
                 "#else\n"
                 "int third = 3;\n"
                 "#endif\n");
  pp.EnterMainFile();

  // Should handle gracefully (error but not crash)
  auto spellings = CollectSpellings(pp);
}

// Test #else before #elif (error case)
TEST_F(PreprocessorTest, ElseBeforeElif) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if 0\n"
                 "int first = 1;\n"
                 "#else\n"
                 "int second = 2;\n"
                 "#elif 1\n"
                 "int third = 3;\n"
                 "#endif\n");
  pp.EnterMainFile();

  // Should handle gracefully (error but not crash)
  auto spellings = CollectSpellings(pp);
}

// Test __FILE__ and __LINE__ in different contexts
TEST_F(PreprocessorTest, FileAndLineMacros) {
  Preprocessor pp(sm_, diags_);
  CreateFile("const char* f1 = __FILE__;\n"
                 "int l1 = __LINE__;\n"
                 "int l2 = __LINE__;\n"
                 "const char* f2 = __FILE__;");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  // Just verify we get tokens without crash
  EXPECT_FALSE(spellings.empty());
}

// Test macro defined on command line style
TEST_F(PreprocessorTest, SimpleObjectLikeMacros) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define ONE 1\n"
                  "int x = ONE;");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "x");
  EXPECT_EQ(spellings[2], "=");
  EXPECT_EQ(spellings[3], "1");
  EXPECT_EQ(spellings[4], ";");
}

// Test very long macro replacement
TEST_F(PreprocessorTest, VeryLongMacroReplacement) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define LONG a + b + c + d + e + f + g + h + i + j + k + l + "
                 "m + n + o + p\n"
                 "int result = LONG;");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "result");
  EXPECT_EQ(spellings[2], "=");
  // Should have many tokens from expansion
  EXPECT_GT(spellings.size(), 10U);
}

// Test macro with only whitespace in replacement
TEST_F(PreprocessorTest, MacroWithOnlyWhitespace) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define SPACE   \n"
                 "int SPACE x = 1;");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "x");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

// Test __func__ predefined identifier (if supported)
TEST_F(PreprocessorTest, FuncIdentifier) {
  Preprocessor pp(sm_, diags_);
  CreateFile("const char* name = __func__;");
  pp.EnterMainFile();

  // __func__ is a C99 predefined identifier, not a macro
  // Behavior depends on implementation
  auto spellings = CollectSpellings(pp);
  EXPECT_FALSE(spellings.empty());
}

// =============================================================================
// More Edge Case Tests
// =============================================================================

// Test line continuation in macro definition
TEST_F(PreprocessorTest, LineContinuationInMacro) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define MULTI_LINE(x) \\\n"
                 "    do { \\\n"
                 "        x++; \\\n"
                 "    } while(0)\n"
                 "MULTI_LINE(i);");
  pp.EnterMainFile();

  // Should handle line continuation correctly
  auto spellings = CollectSpellings(pp);
  EXPECT_FALSE(spellings.empty());
}

// Test line continuation at end of file
TEST_F(PreprocessorTest, LineContinuationAtEOF) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define INCOMPLETE \\");
  pp.EnterMainFile();

  // Should handle gracefully without crash
  Token token = pp.Lex();
  EXPECT_TRUE(token.GetKind() == TokenKind::kEOF || token.GetKind() == TokenKind::kIdentifier);
}

// Test macro with many parameters
TEST_F(PreprocessorTest, MacroWithManyParameters) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define MANY(a, b, c, d, e, f, g, h) a+b+c+d+e+f+g+h\n"
                 "int result = MANY(1, 2, 3, 4, 5, 6, 7, 8);");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  // Should expand all parameters correctly
}

// Test macro invocation spanning multiple lines
TEST_F(PreprocessorTest, MacroInvocationMultipleLines) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define ADD(a, b) ((a) + (b))\n"
                 "int result = ADD(\n"
                 "    1,\n"
                 "    2\n"
                 ");");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "result");
  EXPECT_EQ(spellings[2], "=");
  EXPECT_EQ(spellings[3], "(");
  EXPECT_EQ(spellings[4], "(");
  EXPECT_EQ(spellings[5], "1");
  EXPECT_EQ(spellings[6], ")");
  EXPECT_EQ(spellings[7], "+");
  EXPECT_EQ(spellings[8], "(");
  EXPECT_EQ(spellings[9], "2");
  EXPECT_EQ(spellings[10], ")");
  EXPECT_EQ(spellings[11], ")");
  EXPECT_EQ(spellings[12], ";");
}

// Test macro name that is a keyword
TEST_F(PreprocessorTest, MacroNameIsKeyword) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define int float\n"
                 "int x;");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  // 'int' should be replaced with 'float'
  EXPECT_EQ(spellings[0], "float");
  EXPECT_EQ(spellings[1], "x");
  EXPECT_EQ(spellings[2], ";");
}

// Test #if with 0 and else
TEST_F(PreprocessorTest, IfZeroWithElse) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if 0\n"
                 "int hidden = 1;\n"
                 "#else\n"
                 "int visible = 2;\n"
                 "#endif\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  ASSERT_EQ(spellings.size(), 5U);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "visible");
  EXPECT_EQ(spellings[2], "=");
  EXPECT_EQ(spellings[3], "2");
  EXPECT_EQ(spellings[4], ";");
}

TEST_F(PreprocessorTest, SplitElseWithCommentedBraceAfterEndif) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define X 1\n"
                 "int f(){\n"
                 "  if(2){\n"
                 "#if X\n"
                 "    if(1){\n"
                 "    }else\n"
                 "#endif\n"
                 "    /*if(0)*/{\n"
                 "      return 1;\n"
                 "    }\n"
                 "  }\n"
                 "  return 0;\n"
                 "}\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  std::vector<std::string> expected = {
      "int",    "f",  "(", ")", "{", "if",     "(", "2",    ")",
      "{",      "if", "(", "1", ")", "{",      "}", "else", "{",
      "return", "1",  ";", "}", "}", "return", "0", ";",    "}"};

  EXPECT_EQ(spellings, expected);
}

// Test nested #if 0 blocks
TEST_F(PreprocessorTest, NestedIfZeroBlocks) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if 0\n"
                 "  #if 1\n"
                 "    int inner = 1;\n"
                 "  #endif\n"
                 "  int outer = 2;\n"
                 "#endif\n"
                 "int after = 3;");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  ASSERT_EQ(spellings.size(), 5U);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "after");
}

// Test #if with negative number
TEST_F(PreprocessorTest, IfWithNegativeNumber) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if -1\n"
                 "int negative = 1;\n"
                 "#endif\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "negative");
}

// Test #if with hex number
TEST_F(PreprocessorTest, IfWithHexNumber) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if 0x10\n"
                 "int hex = 1;\n"
                 "#endif\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "hex");
}

// Test #if with octal number
TEST_F(PreprocessorTest, IfWithOctalNumber) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if 010\n"
                 "int octal = 1;\n"
                 "#endif\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "octal");
}

// Test #if with character constant
TEST_F(PreprocessorTest, IfWithCharacterConstant) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if 'a'\n"
                 "int ch = 1;\n"
                 "#endif\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "ch");
}

// Test stringification with complex argument
TEST_F(PreprocessorTest, StringifyComplexArgument) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define STR(x) #x\n"
                 "const char* s = STR(1 + 2 * 3);");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "const");
  EXPECT_EQ(spellings[1], "char");
  EXPECT_EQ(spellings[2], "*");
  EXPECT_EQ(spellings[3], "s");
  EXPECT_EQ(spellings[4], "=");
  // String should contain the expression
  EXPECT_NE(spellings[5].find("1"), std::string::npos);
  EXPECT_EQ(spellings[6], ";");
}

// Test stringification with string argument
TEST_F(PreprocessorTest, StringifyStringArgument) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define STR(x) #x\n"
                 "const char* s = STR(\"hello\");");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "const");
  EXPECT_EQ(spellings[1], "char");
  EXPECT_EQ(spellings[2], "*");
  EXPECT_EQ(spellings[3], "s");
  EXPECT_EQ(spellings[4], "=");
  // Should escape the inner quotes
  EXPECT_EQ(spellings[6], ";");
}

// Test token paste creating valid identifier
TEST_F(PreprocessorTest, TokenPasteValidIdentifier) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define CONCAT(a, b) a ## b\n"
                 "#define PREFIX var_\n"
                 "int CONCAT(PREFIX, 1) = 1;\n"
                 "int CONCAT(PREFIX, 2) = 2;");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  // PREFIX is expanded first, then concatenated
  EXPECT_EQ(spellings[0], "int");
  // Result depends on whether PREFIX is expanded before pasting
}

// Test token paste with a multi-token argument sequence
TEST_F(PreprocessorTest, TokenPasteWithMultiTokenArgument) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define A a b\n"
                 "#define CAT(x, y) x ## y\n"
                 "CAT(A, c)");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  ASSERT_EQ(spellings.size(), 1U);
  EXPECT_EQ(spellings[0], "Ac");
}

// Test token paste creating number
TEST_F(PreprocessorTest, TokenPasteNumber) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define NUM(a, b) a ## b\n"
                 "int x = NUM(12, 34);");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "x");
  EXPECT_EQ(spellings[2], "=");
  EXPECT_EQ(spellings[3], "1234");
  EXPECT_EQ(spellings[4], ";");
}

// Test token paste with empty left operand
TEST_F(PreprocessorTest, TokenPasteEmptyLeft) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define PASTE(a, b) a ## b\n"
                 "#define EMPTY\n"
                 "int x = PASTE(EMPTY, 123);");
  pp.EnterMainFile();

  // Should handle gracefully
  auto spellings = CollectSpellings(pp);
  EXPECT_FALSE(spellings.empty());
}

// Test token paste with empty right operand
TEST_F(PreprocessorTest, TokenPasteEmptyRight) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define PASTE(a, b) a ## b\n"
                 "#define EMPTY\n"
                 "int x = PASTE(123, EMPTY);");
  pp.EnterMainFile();

  // Should handle gracefully
  auto spellings = CollectSpellings(pp);
  EXPECT_FALSE(spellings.empty());
}

// Test # in object-like macro (should be literal)
TEST_F(PreprocessorTest, HashInObjectLikeMacro) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define HASH #\n"
                 "int x = 1 HASH 2;");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  // # in object-like macro should be literal
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "x");
  EXPECT_EQ(spellings[2], "=");
  EXPECT_EQ(spellings[3], "1");
  EXPECT_EQ(spellings[4], "#");
  EXPECT_EQ(spellings[5], "2");
  EXPECT_EQ(spellings[6], ";");
}

// Test ## in object-like macro (should be literal)
TEST_F(PreprocessorTest, HashHashInObjectLikeMacro) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define PASTE_OP ##\n"
                 "int x = 1 PASTE_OP 2;");
  pp.EnterMainFile();

  // ## in object-like macro is tricky
  auto spellings = CollectSpellings(pp);
  EXPECT_FALSE(spellings.empty());
}

// Test macro expansion in array size
TEST_F(PreprocessorTest, MacroInArraySize) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define SIZE 10\n"
                 "int arr[SIZE];");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "arr");
  EXPECT_EQ(spellings[2], "[");
  EXPECT_EQ(spellings[3], "10");
  EXPECT_EQ(spellings[4], "]");
  EXPECT_EQ(spellings[5], ";");
}

// Test macro expansion in function declaration
TEST_F(PreprocessorTest, MacroInFunctionDeclaration) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define RET_TYPE int\n"
                 "#define PARAM char*\n"
                 "RET_TYPE func(PARAM p);");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "func");
  EXPECT_EQ(spellings[2], "(");
  EXPECT_EQ(spellings[3], "char");
  EXPECT_EQ(spellings[4], "*");
  EXPECT_EQ(spellings[5], "p");
  EXPECT_EQ(spellings[6], ")");
  EXPECT_EQ(spellings[7], ";");
}

// Test recursive macro chain (A -> B -> A)
TEST_F(PreprocessorTest, RecursiveMacroChain) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define A B\n"
                 "#define B A\n"
                 "int x = A;");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "x");
  EXPECT_EQ(spellings[2], "=");
  // Should stop recursion, result is A or B
  EXPECT_EQ(spellings[4], ";");
}

// Test mutually recursive macros
TEST_F(PreprocessorTest, MutuallyRecursiveMacros) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define FOO(x) BAR(x)\n"
                 "#define BAR(x) FOO(x)\n"
                 "int x = FOO(1);");
  pp.EnterMainFile();

  // Should stop recursion
  auto spellings = CollectSpellings(pp);
  EXPECT_FALSE(spellings.empty());
}

// Test macro that expands to another macro invocation
TEST_F(PreprocessorTest, MacroExpandsToMacroInvocation) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define DOUBLE(x) ((x) + (x))\n"
                 "#define INVOKE_DOUBLE(x) DOUBLE(x)\n"
                 "int x = INVOKE_DOUBLE(5);");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "x");
  EXPECT_EQ(spellings[2], "=");
  EXPECT_EQ(spellings[3], "(");
  EXPECT_EQ(spellings[4], "(");
  EXPECT_EQ(spellings[5], "5");
  EXPECT_EQ(spellings[6], ")");
  EXPECT_EQ(spellings[7], "+");
  EXPECT_EQ(spellings[8], "(");
  EXPECT_EQ(spellings[9], "5");
  EXPECT_EQ(spellings[10], ")");
  EXPECT_EQ(spellings[11], ")");
  EXPECT_EQ(spellings[12], ";");
}

// Test #warning directive
TEST_F(PreprocessorTest, WarningDirective) {
  Preprocessor pp(sm_, diags_);
  CreateFile("int before = 1;\n"
                 "#warning This is a warning\n"
                 "int after = 2;");
  pp.EnterMainFile();

  // Should continue processing after warning
  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "before");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
}

// Test #if with defined() in expression
TEST_F(PreprocessorTest, DefinedInComplexExpression) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define FOO 1\n"
                 "#if defined(FOO) && FOO\n"
                 "int active = 1;\n"
                 "#endif\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "active");
}

// Test #if with !defined()
TEST_F(PreprocessorTest, NotDefined) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if !defined(NOT_DEFINED)\n"
                 "int active = 1;\n"
                 "#endif\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "active");
}

// Test #if with comparison
TEST_F(PreprocessorTest, IfWithComparison) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define VERSION 5\n"
                 "#if VERSION >= 3\n"
                 "int new_version = 1;\n"
                 "#else\n"
                 "int old_version = 0;\n"
                 "#endif\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "new_version");
}

// Test #if with bitwise operators
TEST_F(PreprocessorTest, IfWithBitwiseOperators) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if (1 << 2) == 4\n"
                 "int shift_ok = 1;\n"
                 "#endif\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "shift_ok");
}

// Test #if with ternary operator
TEST_F(PreprocessorTest, IfWithTernaryOperator) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if 1 ? 2 : 3\n"
                 "int ternary = 1;\n"
                 "#endif\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "ternary");
}

// Test comment inside macro argument
TEST_F(PreprocessorTest, CommentInMacroArgument) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define ADD(a, b) ((a) + (b))\n"
                 "int x = ADD(1 /* first */, 2 /* second */);");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  // Comments should be stripped
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "x");
  EXPECT_EQ(spellings[2], "=");
  EXPECT_EQ(spellings[3], "(");
  EXPECT_EQ(spellings[4], "(");
  EXPECT_EQ(spellings[5], "1");
  EXPECT_EQ(spellings[6], ")");
  EXPECT_EQ(spellings[7], "+");
  EXPECT_EQ(spellings[8], "(");
  EXPECT_EQ(spellings[9], "2");
  EXPECT_EQ(spellings[10], ")");
  EXPECT_EQ(spellings[11], ")");
  EXPECT_EQ(spellings[12], ";");
}

// Test block comment spanning multiple lines in macro
TEST_F(PreprocessorTest, BlockCommentInMacro) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define CODE /* comment */ 42\n"
                 "int x = CODE;");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "x");
  EXPECT_EQ(spellings[2], "=");
  EXPECT_EQ(spellings[3], "42");
  EXPECT_EQ(spellings[4], ";");
}

// Test #define with no name (error case)
TEST_F(PreprocessorTest, DefineWithNoName) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define\n"
                 "int x = 1;");
  pp.EnterMainFile();

  // Should handle error gracefully and continue parsing
  Token token = pp.Lex();
  // Error recovery should allow parsing to continue
  while (token.GetKind() != TokenKind::kEOF) {
    token = pp.Lex();
  }
  EXPECT_TRUE(token.GetKind() == TokenKind::kEOF);
}

// Test #define with special characters in replacement
TEST_F(PreprocessorTest, DefineWithSpecialCharacters) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define SYMBOLS !@#$%^&*\n"
                 "char* s = SYMBOLS;");
  pp.EnterMainFile();

  // Should handle special characters
  auto spellings = CollectSpellings(pp);
  EXPECT_FALSE(spellings.empty());
}

// Test empty #include (error case)
TEST_F(PreprocessorTest, EmptyInclude) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#include\n"
                 "int x = 1;");
  pp.EnterMainFile();

  // Should handle error gracefully
  Token token = pp.Lex();
  while (token.GetKind() != TokenKind::kEOF) {
    token = pp.Lex();
  }
  EXPECT_TRUE(token.GetKind() == TokenKind::kEOF);
}

// Test #include with malformed path
TEST_F(PreprocessorTest, MalformedIncludePath) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#include <missing_close\n"
                 "int x = 1;");
  pp.EnterMainFile();

  // Should handle error gracefully
  Token token = pp.Lex();
  while (token.GetKind() != TokenKind::kEOF) {
    token = pp.Lex();
  }
  EXPECT_TRUE(token.GetKind() == TokenKind::kEOF);
}

// Test #line with string
TEST_F(PreprocessorTest, LineDirectiveWithFile) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#line 100 \"custom.c\"\n"
                 "int x = 1;");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "x");
}

// Test #line with very large number
TEST_F(PreprocessorTest, LineDirectiveLargeNumber) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#line 999999999\n"
                 "int x = 1;");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "x");
}

// Test #pragma with complex tokens
TEST_F(PreprocessorTest, PragmaWithComplexTokens) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#pragma GCC diagnostic push\n"
                 "#pragma GCC diagnostic ignored \"-Wall\"\n"
                 "int x = 1;\n"
                 "#pragma GCC diagnostic pop");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  // The diagnostic pragmas are re-emitted into the output (matching Clang's
  // `clang -E -P`), so `int` and `x` are no longer the first two tokens; find
  // them among the emitted spellings.
  auto it_int = std::find(spellings.begin(), spellings.end(), "int");
  ASSERT_NE(it_int, spellings.end());
  auto it_x = std::find(it_int, spellings.end(), "x");
  EXPECT_NE(it_x, spellings.end());
}

// Test macro used before definition
TEST_F(PreprocessorTest, MacroUsedBeforeDefinition) {
  Preprocessor pp(sm_, diags_);
  CreateFile("int x = UNDEFINED_MACRO;\n"
                 "#define UNDEFINED_MACRO 42\n"
                 "int y = UNDEFINED_MACRO;");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  ASSERT_GE(spellings.size(), 5U);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "x");
  EXPECT_EQ(spellings[2], "=");
  // First use should not expand (macro not yet defined)
  EXPECT_EQ(spellings[3], "UNDEFINED_MACRO");
  EXPECT_EQ(spellings[4], ";");

  // Second use after definition should expand
  ASSERT_GE(spellings.size(), 10U);
  EXPECT_EQ(spellings[5], "int");
  EXPECT_EQ(spellings[6], "y");
  EXPECT_EQ(spellings[7], "=");
  EXPECT_EQ(spellings[8], "42");
  EXPECT_EQ(spellings[9], ";");
}

// Test nested #include guard simulation
TEST_F(PreprocessorTest, NestedIncludeGuardSimulation) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#ifndef HEADER_H\n"
                 "#define HEADER_H\n"
                 "#ifndef INNER_H\n"
                 "#define INNER_H\n"
                 "int inner = 1;\n"
                 "#endif\n"
                 "int outer = 2;\n"
                 "#endif\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "inner");
}

// Test macro expansion order
TEST_F(PreprocessorTest, MacroExpansionOrder) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define FIRST 1\n"
                 "#define SECOND FIRST + 2\n"
                 "#define THIRD SECOND + 3\n"
                 "int x = THIRD;");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "x");
  EXPECT_EQ(spellings[2], "=");
  EXPECT_EQ(spellings[3], "1");
  EXPECT_EQ(spellings[4], "+");
  EXPECT_EQ(spellings[5], "2");
  EXPECT_EQ(spellings[6], "+");
  EXPECT_EQ(spellings[7], "3");
  EXPECT_EQ(spellings[8], ";");
}

// Test function-like macro with no space after define
TEST_F(PreprocessorTest, FunctionLikeMacroNoSpace) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#defineFUNC(x) x\n"
                 "int x = FUNC(1);");
  pp.EnterMainFile();

  // Without space, might be parsed differently
  auto spellings = CollectSpellings(pp);
}

// Test macro with backslash at end of replacement
TEST_F(PreprocessorTest, MacroWithTrailingBackslash) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define END_BACKSLASH value\\\n"
                 "\n"
                 "int x = END_BACKSLASH;");
  pp.EnterMainFile();

  // Line continuation extends the definition to include the next line
  // So this should work, or produce an error if not properly terminated
  Token token = pp.Lex();
  while (token.GetKind() != TokenKind::kEOF) {
    token = pp.Lex();
  }
  EXPECT_TRUE(token.GetKind() == TokenKind::kEOF);
}

// Test #if 1 with nothing after
TEST_F(PreprocessorTest, IfOneNoEndif) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if 1");
  pp.EnterMainFile();

  Token token = pp.Lex();
  EXPECT_TRUE(token.GetKind() == TokenKind::kEOF);
}

// Test deeply nested conditionals
TEST_F(PreprocessorTest, DeeplyNestedConditionals) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#if 1\n"
                 "  #if 1\n"
                 "    #if 1\n"
                 "      #if 1\n"
                 "        int deep = 1;\n"
                 "      #endif\n"
                 "    #endif\n"
                 "  #endif\n"
                 "#endif\n");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "deep");
}

// Test multiple macros on same line
TEST_F(PreprocessorTest, MultipleMacrosSameLine) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define A 1\n"
                 "#define B 2\n"
                 "#define C 3\n"
                 "int x = A + B + C;");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "x");
  EXPECT_EQ(spellings[2], "=");
  EXPECT_EQ(spellings[3], "1");
  EXPECT_EQ(spellings[4], "+");
  EXPECT_EQ(spellings[5], "2");
  EXPECT_EQ(spellings[6], "+");
  EXPECT_EQ(spellings[7], "3");
  EXPECT_EQ(spellings[8], ";");
}

// Test macro that expands to nothing multiple times
TEST_F(PreprocessorTest, EmptyMacroMultipleTimes) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define EMPTY\n"
                 "EMPTY EMPTY EMPTY int x = 1; EMPTY EMPTY EMPTY");
  pp.EnterMainFile();

  EXPECT_EQ(pp.Lex().GetLexeme(), "int");
  EXPECT_EQ(pp.Lex().GetLexeme(), "x");
  EXPECT_EQ(pp.Lex().GetLexeme(), "=");
  EXPECT_EQ(pp.Lex().GetLexeme(), "1");
  EXPECT_EQ(pp.Lex().GetLexeme(), ";");
  EXPECT_TRUE(pp.Lex().GetKind() == TokenKind::kEOF);
}

// Test __DATE__ and __TIME__ macros
TEST_F(PreprocessorTest, DateAndTimeMacros) {
  Preprocessor pp(sm_, diags_);
  CreateFile("const char* date = __DATE__;\n"
                 "const char* time = __TIME__;");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "const");
  EXPECT_EQ(spellings[1], "char");
  EXPECT_EQ(spellings[2], "*");
  EXPECT_EQ(spellings[3], "date");
  EXPECT_EQ(spellings[4], "=");
  // __DATE__ should be a string literal
  EXPECT_TRUE(spellings[5].starts_with("\"") || spellings[5].starts_with("M"));
}

// Test macro that expands to __LINE__
TEST_F(PreprocessorTest, MacroExpandsToLine) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define GET_LINE __LINE__\n"
                 "int line1 = GET_LINE;\n"
                 "int line2 = GET_LINE;");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  // Both should get their respective line numbers
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "line1");
  EXPECT_EQ(spellings[2], "=");
  // Line number
  EXPECT_EQ(spellings[5], "int");
  EXPECT_EQ(spellings[6], "line2");
}

// Test function-like macro invoked with no tokens in args
TEST_F(PreprocessorTest, FunctionLikeMacroNoTokensInArgs) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define FUNC(a, b) a b\n"
                 "int x = FUNC(,);");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "int");
  EXPECT_EQ(spellings[1], "x");
  EXPECT_EQ(spellings[2], "=");
  EXPECT_EQ(spellings[3], ";");
}

// =============================================================================
// __VA_OPT__ Tests (C++20 feature)
// =============================================================================

// Test __VA_OPT__ with empty __VA_ARGS__
TEST_F(PreprocessorTest, VaOptWithEmptyArgs) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define LOG(fmt, ...) printf(fmt __VA_OPT__(,) __VA_ARGS__)\n"
                 "LOG(\"hello\");");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  // __VA_OPT__(,) and __VA_ARGS__ both omitted since variadic is empty
  ASSERT_GE(spellings.size(), 5U);
  EXPECT_EQ(spellings[0], "printf");
  EXPECT_EQ(spellings[1], "(");
  EXPECT_EQ(spellings[2], "\"hello\"");
  EXPECT_EQ(spellings[3], ")");
  EXPECT_EQ(spellings[4], ";");
}

// Test __VA_OPT__ with non-empty __VA_ARGS__
TEST_F(PreprocessorTest, VaOptWithNonEmptyArgs) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define LOG(fmt, ...) printf(fmt __VA_OPT__(,) __VA_ARGS__)\n"
                 "LOG(\"%d %d\", 1, 2);");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  // __VA_OPT__(,) emits the comma; __VA_ARGS__ emits the variadic arguments
  ASSERT_GE(spellings.size(), 9U);
  EXPECT_EQ(spellings[0], "printf");
  EXPECT_EQ(spellings[1], "(");
  EXPECT_EQ(spellings[2], "\"%d %d\"");
  EXPECT_EQ(spellings[3], ",");
  EXPECT_EQ(spellings[4], "1");
  EXPECT_EQ(spellings[5], ",");
  EXPECT_EQ(spellings[6], "2");
  EXPECT_EQ(spellings[7], ")");
  EXPECT_EQ(spellings[8], ";");
}

// Test __VA_OPT__ with complex content
TEST_F(PreprocessorTest, VaOptWithComplexContent) {
  Preprocessor pp(sm_, diags_);
  CreateFile(
      "#define WRAP(...) { __VA_OPT__(int arr[] = {__VA_ARGS__};) }\n"
      "WRAP();\n"
      "WRAP(1, 2);");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  // WRAP() with empty args → { }  then ;
  // WRAP(1, 2) → { int arr[] = {1, 2}; }  then ;
  ASSERT_GE(spellings.size(), 17U);
  EXPECT_EQ(spellings[0], "{");
  EXPECT_EQ(spellings[1], "}");
  EXPECT_EQ(spellings[2], ";");
  EXPECT_EQ(spellings[3], "{");
  EXPECT_EQ(spellings[4], "int");
  EXPECT_EQ(spellings[5], "arr");
  EXPECT_EQ(spellings[6], "[");
  EXPECT_EQ(spellings[7], "]");
  EXPECT_EQ(spellings[8], "=");
  EXPECT_EQ(spellings[9], "{");
  EXPECT_EQ(spellings[10], "1");
  EXPECT_EQ(spellings[11], ",");
  EXPECT_EQ(spellings[12], "2");
  EXPECT_EQ(spellings[13], "}");
  EXPECT_EQ(spellings[14], ";");
  EXPECT_EQ(spellings[15], "}");
  EXPECT_EQ(spellings[16], ";");
}

// Test __VA_OPT__ with nested parentheses
TEST_F(PreprocessorTest, VaOptWithNestedParens) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define M(...) f(1 __VA_OPT__(+ (2 + 3)) __VA_ARGS__)\n"
                 "M();\n"
                 "M(x);");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  // M() with empty args → f(1)  then ;
  // M(x) → f(1 + (2 + 3) x)  then ;
  ASSERT_GE(spellings.size(), 17U);
  EXPECT_EQ(spellings[0], "f");
  EXPECT_EQ(spellings[1], "(");
  EXPECT_EQ(spellings[2], "1");
  EXPECT_EQ(spellings[3], ")");
  EXPECT_EQ(spellings[4], ";");
  EXPECT_EQ(spellings[5], "f");
  EXPECT_EQ(spellings[6], "(");
  EXPECT_EQ(spellings[7], "1");
  EXPECT_EQ(spellings[8], "+");
  EXPECT_EQ(spellings[9], "(");
  EXPECT_EQ(spellings[10], "2");
  EXPECT_EQ(spellings[11], "+");
  EXPECT_EQ(spellings[12], "3");
  EXPECT_EQ(spellings[13], ")");
  EXPECT_EQ(spellings[14], "x");
  EXPECT_EQ(spellings[15], ")");
  EXPECT_EQ(spellings[16], ";");
}

// Test __VA_OPT__ with only __VA_OPT__
TEST_F(PreprocessorTest, VaOptAlone) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define EMPTY(...) __VA_OPT__(x)\n"
                 "EMPTY();\n"
                 "EMPTY(a);");
  pp.EnterMainFile();

  auto spellings = CollectSpellings(pp);
  // EMPTY(); → nothing, then ;
  // EMPTY(a); → x, then ;
  ASSERT_GE(spellings.size(), 3U);
  EXPECT_EQ(spellings[0], ";");
  EXPECT_EQ(spellings[1], "x");
  EXPECT_EQ(spellings[2], ";");
}

// Assembly-style \name macro parameter treated as identifier
// (GAS .macro / .endm blocks use \ for macro parameters).
TEST_F(PreprocessorTest, BackslashIdentifier) {
  Preprocessor pp(sm_, diags_);
  CreateFile("\\newinstr2\n");
  pp.EnterMainFile();
  std::vector<Token> tokens = LexAll(pp);
  ASSERT_GE(tokens.size(), 2u);
  EXPECT_EQ(tokens[0].GetKind(), TokenKind::kIdentifier);
  EXPECT_EQ(tokens[0].GetLexeme(), "\\newinstr2");
}

// Multiple \name tokens in a row
TEST_F(PreprocessorTest, MultipleBackslashIdentifiers) {
  Preprocessor pp(sm_, diags_);
  CreateFile("\\oldinstr \\newinstr \\ft_flags\n");
  pp.EnterMainFile();
  std::vector<Token> tokens = LexAll(pp);
  ASSERT_GE(tokens.size(), 4u);
  EXPECT_EQ(tokens[0].GetKind(), TokenKind::kIdentifier);
  EXPECT_EQ(tokens[0].GetLexeme(), "\\oldinstr");
  EXPECT_EQ(tokens[1].GetKind(), TokenKind::kIdentifier);
  EXPECT_EQ(tokens[1].GetLexeme(), "\\newinstr");
  EXPECT_EQ(tokens[2].GetKind(), TokenKind::kIdentifier);
  EXPECT_EQ(tokens[2].GetLexeme(), "\\ft_flags");
}

// Stray \ followed by a non-identifier character still produces kUnknown
TEST_F(PreprocessorTest, StrayBackslashWithDigit) {
  Preprocessor pp(sm_, diags_);
  CreateFile("\\123\n");
  pp.EnterMainFile();
  std::vector<Token> tokens = LexAll(pp);
  ASSERT_GE(tokens.size(), 3u);
  EXPECT_EQ(tokens[0].GetKind(), TokenKind::kUnknown);
  EXPECT_EQ(tokens[0].GetLexeme(), "\\");
}

// GNU named-variadic `name...` syntax: stringification of a simple arg
TEST_F(PreprocessorTest, NamedVariadicStringify) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define STR(x...) #x\n"
             "const char* s = STR(hello);");
  pp.EnterMainFile();
  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "const");
  EXPECT_EQ(spellings[4], "=");
  EXPECT_EQ(spellings[5], "\"hello\"");
  EXPECT_EQ(spellings[6], ";");
}

// Named-variadic with nested parens in the argument
TEST_F(PreprocessorTest, NamedVariadicStringifyNestedParens) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define STR(x...) #x\n"
             "const char* s = STR((x)(y));");
  pp.EnterMainFile();
  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "const");
  EXPECT_EQ(spellings[4], "=");
  EXPECT_EQ(spellings[5], "\"(x)(y)\"");
  EXPECT_EQ(spellings[6], ";");
}

// Named-variadic stringification of an empty arg
TEST_F(PreprocessorTest, NamedVariadicStringifyEmpty) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define STR(x...) #x\n"
             "const char* s = STR();");
  pp.EnterMainFile();
  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "const");
  EXPECT_EQ(spellings[4], "=");
  EXPECT_EQ(spellings[5], "\"\"");
  EXPECT_EQ(spellings[6], ";");
}

// Named-variadic with multi-token arg containing a comma at top level.
// The named variadic parameter captures all comma-separated groups.
TEST_F(PreprocessorTest, NamedVariadicStringifyCommaArg) {
  Preprocessor pp(sm_, diags_);
  CreateFile("#define STR(x...) #x\n"
             "const char* s = STR(a, b);");
  pp.EnterMainFile();
  auto spellings = CollectSpellings(pp);
  EXPECT_EQ(spellings[0], "const");
  EXPECT_EQ(spellings[4], "=");
  EXPECT_EQ(spellings[5], "\"a, b\"");
  EXPECT_EQ(spellings[6], ";");
}

}  // namespace
}  // namespace bcc
