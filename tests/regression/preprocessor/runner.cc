#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "bcc/basic/diagnostic.hh"
#include "bcc/basic/file_manager.hh"
#include "bcc/basic/source_manager.hh"
#include "bcc/lex/token.hh"
#include "bcc/lex/token_kind.hh"
#include "bcc/pp/header_search.hh"
#include "bcc/pp/preprocessor.hh"
#include "gtest/gtest.h"

namespace bcc {
namespace {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Normalisation helpers — remove irrelevant differences between preprocessor
// outputs (line markers, whitespace, blank lines).
// ---------------------------------------------------------------------------

// Remove Clang/GCC-style linemarkers:  "# N "file.c" ..." (one per line).
// Hand-rolled to be binary-safe (std::regex is not NUL-safe).
static std::string StripLineMarkers(const std::string& s) {
  std::string out;
  std::size_t i = 0;
  const std::size_t n = s.size();

  while (i < n) {
    // Find the end of the current line.
    std::size_t e = s.find('\n', i);
    std::size_t line_end = (e == std::string::npos) ? n : e;  // exclusive
    // A line marker starts with "# <digits> ".
    std::size_t p = i;

    if (p < line_end && s[p] == '#') {
      ++p;

      if (p < line_end && s[p] == ' ') {
        ++p;
        std::size_t d = p;
        while (d < line_end && s[d] >= '0' && s[d] <= '9') ++d;
        if (d > p && d < line_end && s[d] == ' ') {
          // It's a line marker; skip the whole line (and its newline).
          i = (e == std::string::npos) ? n : e + 1;
          continue;
        }
      }
    }
    out.append(s, i, line_end - i);
    if (e != std::string::npos) out += '\n';
    i = (e == std::string::npos) ? n : e + 1;
  }
  return out;
}

// Collapse runs of horizontal whitespace into a single space. Binary-safe.
static std::string CollapseHorizontalWS(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool in_ws = false;
  for (char c : s) {
    if (c == ' ' || c == '\t') {
      in_ws = true;
    } else {
      if (in_ws) {
        out += ' ';
        in_ws = false;
      }
      out += c;
    }
  }
  if (in_ws) out += ' ';
  return out;
}

// Collapse consecutive newlines into a single newline. Binary-safe.
static std::string CollapseBlankLines(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  std::size_t i = 0;
  const std::size_t n = s.size();

  while (i < n) {
    if (s[i] == '\n') {
      out += '\n';
      while (i < n && s[i] == '\n') ++i;
    } else {
      out += s[i++];
    }
  }

  return out;
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
  // fread is binary-safe: fgets + std::string += (const char*) would truncate
  // the captured output at the first NUL byte (e.g. NULs in string literals).
  std::size_t n;

  while ((n = std::fread(buf, 1, sizeof(buf), pipe)) > 0) {
    result.append(buf, n);
  }

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
// sequence when concatenated without any separator. Mirrors Clang's
// AvoidConcat: a space is needed when the previous spelling extended by the
// first character of the next forms a longer punctuator, or when two
// identifier/number characters meet.
static bool WouldMerge(const Token& prev, const Token& next) {
  std::string_view prev_spelling = prev.GetLexeme();
  std::string_view next_spelling = next.GetLexeme();

  if (prev_spelling.empty() || next_spelling.empty()) return false;

  char p = prev_spelling.back();
  char n = next_spelling.front();

  // Two identifier/number characters -> a longer identifier/number.
  if ((std::isalnum(static_cast<unsigned char>(p)) || p == '_') &&
      (std::isalnum(static_cast<unsigned char>(n)) || n == '_'))
    return true;

  // Digit after period -> a number (e.g. `1 .2` -> `1.2`).
  if (p == '.' && std::isdigit(static_cast<unsigned char>(n))) return true;

  // An encoding-prefix identifier immediately followed by a string/char
  // literal would re-lex as a single prefixed literal.
  if ((n == '"' || n == '\'') && !prev_spelling.empty()) {
    std::string_view pre = prev_spelling;
    if (pre == "L" || pre == "u" || pre == "U" || pre == "u8" || pre == "R" ||
        pre == "LR" || pre == "uR" || pre == "UR" || pre == "u8R") {
      return true;
    }
  }

  // Punctuator merge: does prev extended by the first char of next form a valid
  // (longer) punctuator? This correctly avoids inserting a space between
  // `<<` and `<<` (which stay two tokens) while inserting one between `<` and
  // `<` (which would form `<<`).
  static const std::unordered_set<std::string_view> kPunctuators = {
      "<<=", ">>=", "...", "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=",
      "&&",  "||",  "*=",  "/=", "%=", "+=", "-=", "&=", "^=", "|=", "##"};
  std::string cand;
  cand.reserve(prev_spelling.size() + 1);
  cand.append(prev_spelling);
  cand.push_back(n);

  if (kPunctuators.count(cand)) return true;

  return false;
}

// Strip backslash-newline line splices from a raw lexeme. Tokens that span a
// splice carry kNeedsCleaning; clang -E joins them, so bcc must too.
static std::string CleanLexeme(const Token& t) {
  std::string_view raw = t.GetLexeme();

  // Clang decodes \uXXXX/\UXXXXXXXX UCNs in identifier spellings when printing
  // -E output; mirror that for identifier tokens.
  if (t.GetKind() == TokenKind::kIdentifier && !t.NeedsCleaning()) {
    return DecodeIdentifierUCNs(raw);
  }

  if (!t.NeedsCleaning()) return std::string(raw);

  std::string out;
  out.reserve(raw.size());

  for (std::size_t i = 0; i < raw.size();) {
    if (raw[i] == '\\' && i + 1 < raw.size()) {
      std::size_t j = i + 1;

      while (j < raw.size() && (raw[j] == ' ' || raw[j] == '\t')) ++j;

      if (j < raw.size() && (raw[j] == '\n' || raw[j] == '\r')) {
        char nl = raw[j];
        i = j + 1;

        if (nl == '\r' && i < raw.size() && raw[i] == '\n') ++i;

        continue;
      }
    }
    out += raw[i++];
  }
  return out;
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

    out.append(CleanLexeme(t));
    prev = std::move(t);
  }

  return {Normalize(out), diags.NumErrors()};
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class PreprocessorRegressionTest : public ::testing::TestWithParam<fs::path> {
 protected:
  void SetUp() override {
    temp_dir_ = fs::temp_directory_path() / "bcc_pp_reg_test";
    fs::remove_all(temp_dir_);
    fs::create_directories(temp_dir_);

    src_ = GetParam();
    rel_ = fs::relative(src_, fs::path(TEST_DATA_DIR));

    // Copy the test file's whole source tree (siblings, the Inputs/ directory
    // of helper headers, and sibling .c files that may be #included) into the
    // temp dir so relative includes resolve identically for clang and bcc.
    fs::path src_dir = src_.parent_path();
    fs::copy(
        src_dir, temp_dir_,
        fs::copy_options::recursive | fs::copy_options::overwrite_existing);
  }

  void TearDown() override { fs::remove_all(temp_dir_); }

  fs::path temp_dir_;
  fs::path src_;
  fs::path rel_;
};

TEST_P(PreprocessorRegressionTest, OutputMatchesClang) {
  fs::path test_file = temp_dir_ / src_.filename();

  SCOPED_TRACE("File: " + rel_.string());

  auto bcc = PreprocessWithBcc(test_file, temp_dir_);

  // First, verify that the bcc preprocessor didn't crash.
  ASSERT_NE(bcc.errors, 999u) << "Failed to open file";

  std::string clang_out = RunClang(test_file, temp_dir_);

  if (bcc.output != clang_out && std::getenv("BCC_PP_DIFF")) {
    std::cerr << "\n===== DIFF (clang <  bcc >) : " << rel_.string()
              << " =====\n";
    std::istringstream a(clang_out), b(bcc.output);
    std::string la, lb;
    unsigned ln = 1;

    for (;;) {
      bool ea = !std::getline(a, la);
      bool eb = !std::getline(b, lb);

      if (ea && eb) break;
      if (ea) {
        std::cerr << ln << " > " << lb << "\n";
        continue;
      }
      if (eb) {
        std::cerr << ln << " < " << la << "\n";
        ln++;
        continue;
      }
      if (la != lb) {
        std::cerr << ln << " < " << la << "\n";
        std::cerr << ln << " > " << lb << "\n";
      }
      ln++;
    }
    std::cerr << "===== END " << rel_.string() << " =====\n";
  }
  EXPECT_EQ(bcc.output, clang_out);
}

// Discover all .c test files recursively under TEST_DATA_DIR.
static auto DiscoverTests() {
  std::vector<fs::path> files;
  fs::path root(TEST_DATA_DIR);

  if (!fs::exists(root)) return files;

  const char* filter = std::getenv("BCC_PP_FILTER");

  for (auto& entry : fs::recursive_directory_iterator(root)) {
    if (entry.path().extension() == ".c") {
      if (filter == nullptr ||
          entry.path().string().find(filter) != std::string::npos)
        files.push_back(entry.path());
    }
  }

  std::sort(files.begin(), files.end());

  return files;
}

INSTANTIATE_TEST_SUITE_P(PreprocessorRegression, PreprocessorRegressionTest,
                         ::testing::ValuesIn(DiscoverTests()));

}  // namespace
}  // namespace bcc
