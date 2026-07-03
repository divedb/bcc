#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "bcc/basic/diagnostic.hh"
#include "bcc/basic/file_manager.hh"
#include "bcc/basic/source_manager.hh"
#include "bcc/pp/header_search.hh"
#include "bcc/pp/preprocessor.hh"
#include "bcc/lex/token.hh"
#include "bcc/lex/token_kind.hh"
#include "gtest/gtest.h"

namespace bcc {
namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Normalisation helpers — remove irrelevant differences between preprocessor
// outputs (line markers, whitespace, blank lines).
// ---------------------------------------------------------------------------

// Remove Clang/GCC-style linemarkers:  "# 1 "file.c" ..."
static std::string StripLineMarkers(const std::string& s) {
  static const std::regex re(
      "^# [0-9]+ \"[^\"]*\"( [0-9]+)?( [0-9]+)?( [0-9]+)?( [0-9]+)?\n",
      std::regex::multiline);
  return std::regex_replace(s, re, "");
}

// Collapse runs of horizontal whitespace into a single space.
static std::string CollapseHorizontalWS(const std::string& s) {
  static const std::regex re("[ \t]+");
  return std::regex_replace(s, re, " ");
}

// Collapse consecutive blank lines so differences in whitespace-only lines
// between preprocessors (e.g. at file-transition boundaries) do not cause
// spurious failures.
static std::string CollapseBlankLines(const std::string& s) {
  static const std::regex re("\n{2,}");
  return std::regex_replace(s, re, "\n");
}

// Trim leading/trailing blank lines and trailing whitespace per line.
static std::string TrimLines(const std::string& s) {
  std::string out;
  std::istringstream stream(s);
  std::string line;
  bool has_content = false;
  while (std::getline(stream, line)) {
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    if (!has_content && line.empty()) continue;
    has_content = true;
    out += line + "\n";
  }
  while (!out.empty() && out.back() == '\n') out.pop_back();
  if (!out.empty()) out += '\n';
  return out;
}

static std::string Normalize(const std::string& s) {
  std::string out = s;
  out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
  out = StripLineMarkers(out);
  out = CollapseHorizontalWS(out);
  out = CollapseBlankLines(out);
  out = TrimLines(out);
  return out;
}

// ---------------------------------------------------------------------------
// Clang baseline
// ---------------------------------------------------------------------------

static std::string RunClang(const fs::path& file, const fs::path& inc_dir) {
  std::string cmd = "clang -E -P -std=gnu17 -I" + inc_dir.string() + " " +
                    file.string() + " 2>/dev/null";
  FILE* pipe = popen(cmd.c_str(), "r");
  if (!pipe) return {};
  std::string result;
  char buf[4096];
  while (fgets(buf, sizeof(buf), pipe)) result += buf;
  pclose(pipe);
  return Normalize(result);
}

// ---------------------------------------------------------------------------
// bcc preprocessor runner
// ---------------------------------------------------------------------------

struct BccResult {
  std::string output;
  unsigned errors;
};

// Returns true if two adjacent tokens would merge into a different token
// sequence when concatenated without any separator.
static bool WouldMerge(const Token& prev, const Token& next) {
  std::string_view prev_spelling = prev.GetLexeme();
  std::string_view next_spelling = next.GetLexeme();
  if (prev_spelling.empty() || next_spelling.empty()) return false;

  char p = prev_spelling.back();
  char n = next_spelling.front();

  // Two identifier/number characters → would form a longer identifier/number.
  if ((std::isalnum(static_cast<unsigned char>(p)) || p == '_') &&
      (std::isalnum(static_cast<unsigned char>(n)) || n == '_'))
    return true;

  // Digit after period → would form a number (e.g. `1 .2` → `1.2`).
  if (p == '.' && std::isdigit(static_cast<unsigned char>(n))) return true;

  // Two-character punctuator merge (C / C++ standard tokens).
  switch (p) {
    case '+': if (n == '+' || n == '=') return true; break;
    case '-': if (n == '-' || n == '=' || n == '>') return true; break;
    case '<': if (n == '<' || n == '=') return true; break;
    case '>': if (n == '>' || n == '=') return true; break;
    case '=': if (n == '=') return true; break;
    case '!': if (n == '=') return true; break;
    case '&': if (n == '&' || n == '=') return true; break;
    case '|': if (n == '|' || n == '=') return true; break;
    case '*': if (n == '=') return true; break;
    case '/': if (n == '/' || n == '*' || n == '=') return true; break;
    case '%': if (n == '=') return true; break;
    case '^': if (n == '=') return true; break;
    case ':': if (n == ':') return true; break;
    case '#': if (n == '#') return true; break;
  }
  return false;
}

static BccResult PreprocessWithBcc(const fs::path& file,
                                   const fs::path& inc_dir) {
  IgnoringDiagConsumer consumer;
  FileSystemOptions fs_opts{inc_dir.string()};
  FileManager fm(fs_opts);
  SourceManager sm(fm);
  DiagnosticsEngine diags(&consumer, &sm);

  HeaderSearch hs(fm);
  hs.AddAngledSearchPath(inc_dir.string());

  const FileEntry* fe = fm.GetFile(file.string());
  if (!fe) return {{}, 999};

  FileID fid = sm.CreateFileID(*fe);
  sm.SetMainFileID(fid);

  Preprocessor pp(sm, diags);
  pp.SetHeaderSearch(hs);
  pp.EnterMainFile();

  std::string out;
  Token prev{SourceLocation{}, TokenKind::kUnknown, nullptr, 0u};
  for (;;) {
    Token t = pp.Lex();
    if (t.GetKind() == TokenKind::kEOF) break;

    if (!out.empty()) {
      if (t.IsStartOfLine()) {
        out += '\n';
        if (t.HasLeadingSpace()) out += ' ';
      } else if (out.back() != '\n') {
        if (t.HasLeadingSpace()) {
          out += ' ';
        } else if (WouldMerge(prev, t)) {
          out += ' ';
        }
      }
    } else if (t.HasLeadingSpace()) {
      out += ' ';
    }

    out.append(t.GetLexeme().data(), t.GetLexeme().size());
    prev = std::move(t);
  }

  return {Normalize(out), diags.NumErrors()};
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class LexerRegressionTest : public ::testing::TestWithParam<fs::path> {
 protected:
  void SetUp() override {
    temp_dir_ = fs::temp_directory_path() / "bcc_lex_reg_test";
    fs::remove_all(temp_dir_);
    fs::create_directories(temp_dir_);

    src_ = GetParam();
    rel_ = fs::relative(src_, fs::path(TEST_DATA_DIR));

    // Copy the test file and any sibling .h/.def files into the temp dir.
    fs::path src_dir = src_.parent_path();
    for (auto& e : fs::directory_iterator(src_dir)) {
      auto ext = e.path().extension();
      if (ext == ".h" || ext == ".def" || e.path() == src_) {
        fs::copy(e.path(), temp_dir_ / e.path().filename(),
                 fs::copy_options::overwrite_existing);
      }
    }
  }

  void TearDown() override { fs::remove_all(temp_dir_); }

  fs::path temp_dir_;
  fs::path src_;
  fs::path rel_;
};

TEST_P(LexerRegressionTest, OutputMatchesClang) {
  fs::path test_file = temp_dir_ / src_.filename();

  SCOPED_TRACE("File: " + rel_.string());

  auto bcc = PreprocessWithBcc(test_file, temp_dir_);

  // First, verify that the bcc preprocessor didn't crash.
  ASSERT_NE(bcc.errors, 999u) << "Failed to open file";

  std::string clang_out = RunClang(test_file, temp_dir_);

  EXPECT_EQ(bcc.output, clang_out);
}

// Discover all .c test files recursively under TEST_DATA_DIR.
static auto DiscoverTests() {
  std::vector<fs::path> files;
  fs::path root(TEST_DATA_DIR);
  if (!fs::exists(root)) return files;
  for (auto& entry : fs::recursive_directory_iterator(root)) {
    if (entry.path().extension() == ".c") files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());
  return files;
}

INSTANTIATE_TEST_SUITE_P(LexerRegression, LexerRegressionTest,
                         ::testing::ValuesIn(DiscoverTests()));

}  // namespace
}  // namespace bcc
