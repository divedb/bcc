// pp_dump — a minimal `-E` driver mirroring token_dump.
//
// Reads a translation unit, runs it through the Preprocessor, and prints the
// fully preprocessed text (macros expanded, directives consumed, includes
// spliced in), with GNU-style line markers so the output tracks presumed
// locations.
//
// Supported flags:
//   -I<dir>   add a system (angled) include search path
//   -P        inhibit generation of line markers
//   -C        keep comments in the preprocessed output
//   -dM       print all active macro definitions (instead of normal output)
//   -dD       keep #define directives in the normal output, at their source
//   -M        write a Makefile dependency rule (all headers, incl. system)
//   -MM       like -M but omit system headers
//   -MT tgt   dependency rule target (default: <input>.o)

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bcc/basic/characteristic_kind.hh"
#include "bcc/basic/diagnostic.hh"
#include "bcc/basic/file_id.hh"
#include "bcc/basic/file_manager.hh"
#include "bcc/basic/presumed_loc.hh"
#include "bcc/basic/source_manager.hh"
#include "bcc/basic/text_diagnostic_printer.hh"
#include "bcc/lex/token.hh"
#include "bcc/lex/token_kind.hh"
#include "bcc/pp/header_search.hh"
#include "bcc/pp/macro_info.hh"
#include "bcc/pp/macro_print.hh"
#include "bcc/pp/pp_callbacks.hh"
#include "bcc/pp/preprocessor.hh"

namespace {

struct Options {
  bool no_line_markers = false;  // -P
  bool dump_macros = false;      // -dM
  bool retain_defines = false;   // -dD
  bool keep_comments = false;    // -C
  bool dep_makefile = false;     // -M or -MM
  bool dep_user_only = false;    // -MM (exclude system headers)
  std::string dep_target;        // -MT
  std::vector<std::string> include_dirs;
  const char* input_path = nullptr;
};

Options ParseArgs(int argc, char* argv[]) {
  Options opts;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "-E") {
      // Preprocessed output is the default mode; accepted for GCC compat.
    } else if (arg == "-P") {
      opts.no_line_markers = true;
    } else if (arg == "-C") {
      opts.keep_comments = true;
    } else if (arg == "-dM") {
      opts.dump_macros = true;
    } else if (arg == "-dD") {
      opts.retain_defines = true;
    } else if (arg == "-M") {
      opts.dep_makefile = true;
      opts.dep_user_only = false;
    } else if (arg == "-MM") {
      opts.dep_makefile = true;
      opts.dep_user_only = true;
    } else if (arg == "-MT" && i + 1 < argc) {
      opts.dep_target = argv[++i];
    } else if (arg.rfind("-MT", 0) == 0) {
      opts.dep_target = std::string(arg.substr(3));
    } else if (arg.rfind("-I", 0) == 0) {
      opts.include_dirs.emplace_back(arg.substr(2));
    } else {
      opts.input_path = argv[i];
    }
  }
  return opts;
}

// Removes backslash-newline line splices from a raw lexeme so the emitted text
// is the token's logical spelling.
std::string CleanLexeme(std::string_view raw) {
  if (raw.find('\\') == std::string_view::npos) return std::string(raw);
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

// Builds the default dependency target for \p input: its basename with the
// extension replaced by ".o".
std::string DefaultTarget(std::string_view input) {
  std::string_view base = input;
  if (std::string_view::size_type slash = base.find_last_of('/');
      slash != std::string_view::npos) {
    base = base.substr(slash + 1);
  }
  std::string out(base);
  if (std::string_view::size_type dot = out.find_last_of('.');
      dot != std::string::npos) {
    out.resize(dot);
  }
  out += ".o";
  return out;
}

// A recorded #define for -dD, anchored at its definition's presumed location so
// it can be interleaved with the preprocessed output.
struct RecordedDefine {
  std::string text;
  std::string file;
  unsigned line;
};

// Records every #define as a formatted line. Installed as the preprocessor's
// single PPCallbacks when -dD is active; the TextPrinter drains the records.
class DefineRecorder : public bcc::PPCallbacks {
 public:
  DefineRecorder(bcc::SourceManager& sm, std::vector<RecordedDefine>& out)
      : sm_(sm), out_(out) {}

  void MacroDefined(const bcc::IdentifierInfo* name,
                    const bcc::MacroInfo* macro) override {
    bcc::PresumedLoc pl = sm_.GetPresumedLoc(macro->GetDefinitionLoc());
    out_.push_back(
        {bcc::FormatMacroDefine(*name, *macro),
         std::string(pl.IsValid() ? pl.filename : std::string_view{}),
         pl.IsValid() ? pl.line : 0u});
  }

 private:
  bcc::SourceManager& sm_;
  std::vector<RecordedDefine>& out_;
};

// Collects resolved header paths for -M / -MM dependency output.
class DepCollector : public bcc::PPCallbacks {
 public:
  explicit DepCollector(bool user_only) : user_only_(user_only) {}

  void InclusionDirective(bcc::SourceLocation, std::string_view, bool,
                          const bcc::FileEntry* file,
                          bcc::CharacteristicKind file_type) override {
    if (file == nullptr) return;
    // -MM omits system headers.
    if (user_only_ && bcc::IsSystemHeader(file_type)) return;
    std::string path = file->GetName();
    if (seen_.insert(path).second) deps_.push_back(std::move(path));
  }

  std::vector<std::string> deps_;

 private:
  bool user_only_;
  std::set<std::string> seen_;
};

// Extracts /* ... */ and // ... comment text from buf[a, b).
std::vector<std::string> ExtractComments(std::string_view buf, std::size_t a,
                                         std::size_t b) {
  std::vector<std::string> out;
  std::size_t i = a;
  while (i + 1 < b) {
    if (buf[i] != '/') {
      ++i;
      continue;
    }
    if (buf[i + 1] == '/') {
      std::size_t e = i + 2;
      while (e < b && buf[e] != '\n') ++e;
      out.emplace_back(buf.substr(i, e - i));
      i = e;
    } else if (buf[i + 1] == '*') {
      std::size_t e = i + 2;
      while (e < b && !(buf[e - 1] == '*' && buf[e] == '/')) ++e;
      if (e < b) ++e;  // include the closing '/'
      out.emplace_back(buf.substr(i, e - i));
      i = e;
    } else {
      ++i;
    }
  }
  return out;
}

/// Reconstructs -E text from the preprocessed token stream. -P suppresses line
/// markers; -C re-emits comments found in source gaps; -dD interleaves recorded
/// #define directives at their definition location.
class TextPrinter {
 public:
  TextPrinter(bcc::SourceManager& sm, std::ostream& os, bool no_markers,
              bool keep_comments, std::vector<RecordedDefine>* defines)
      : sm_(sm),
        os_(os),
        no_markers_(no_markers),
        keep_comments_(keep_comments),
        defines_(defines) {}

  void Emit(const bcc::Token& tok) {
    bcc::PresumedLoc p =
        sm_.GetPresumedLoc(sm_.GetExpansionLoc(tok.GetLocation()));
    unsigned line = p.IsValid() ? p.line : line_;
    std::string file = p.IsValid() ? std::string(p.filename) : file_;

    bool file_changed = (file != file_);

    if (keep_comments_) EmitCommentsBefore(tok);

    if (first_ || file_changed) {
      if (!first_) os_ << '\n';
      if (!no_markers_) LineMarker(line, file);
    } else if (line > line_) {
      if (no_markers_ || line - line_ <= 8) {
        for (unsigned i = line_; i < line; ++i) os_ << '\n';
      } else {
        os_ << '\n';
        LineMarker(line, file);
      }
    } else if (line < line_) {
      os_ << '\n';
      if (!no_markers_) LineMarker(line, file);
    } else if (tok.HasLeadingSpace()) {
      os_ << ' ';
    }

    if (file_changed && !first_) FlushDefinesForFile(file_);
    FlushDefinesUpTo(file, line);

    os_ << CleanLexeme(tok.GetLexeme());
    line_ = line;
    file_ = file;
    first_ = false;
  }

  void Finish() {
    FlushDefinesForFile(file_);
    FlushAllRemaining();
    if (!first_) os_ << '\n';
  }

 private:
  void LineMarker(unsigned line, std::string_view file) {
    os_ << "# " << line << " \"" << file << "\"\n";
    line_ = line;
    file_ = std::string(file);
  }

  // -C: emit any comments in the source gap preceding this token. Only genuine
  // file tokens are scanned (macro/scratch tokens have no source comments).
  void EmitCommentsBefore(const bcc::Token& tok) {
    bcc::SourceLocation loc = tok.GetLocation();
    if (sm_.GetSpellingLoc(loc) != sm_.GetExpansionLoc(loc)) return;
    auto [fid, off] = sm_.GetDecomposedSpellingLoc(loc);
    if (!fid.IsValid()) return;
    std::string_view buf = sm_.GetBufferData(fid);
    if (last_comment_fid_ == fid && off >= last_comment_end_ &&
        off <= buf.size()) {
      for (auto& c : ExtractComments(buf, last_comment_end_, off)) {
        os_ << c << '\n';
      }
    }
    last_comment_fid_ = fid;
    last_comment_end_ = off + tok.GetLexeme().size();
  }

  void EnsureEmittedSize() {
    if (defines_ && define_emitted_.size() < defines_->size()) {
      define_emitted_.resize(defines_->size(), 0);
    }
  }

  void FlushDefinesUpTo(const std::string& file, unsigned line) {
    if (!defines_) return;
    EnsureEmittedSize();
    for (std::size_t i = 0; i < defines_->size(); ++i) {
      if (!define_emitted_[i] && (*defines_)[i].file == file &&
          (*defines_)[i].line <= line) {
        os_ << (*defines_)[i].text << '\n';
        define_emitted_[i] = 1;
      }
    }
  }

  void FlushDefinesForFile(const std::string& file) {
    if (!defines_) return;
    EnsureEmittedSize();
    for (std::size_t i = 0; i < defines_->size(); ++i) {
      if (!define_emitted_[i] && (*defines_)[i].file == file) {
        os_ << (*defines_)[i].text << '\n';
        define_emitted_[i] = 1;
      }
    }
  }

  void FlushAllRemaining() {
    if (!defines_) return;
    EnsureEmittedSize();
    for (std::size_t i = 0; i < defines_->size(); ++i) {
      if (!define_emitted_[i]) {
        os_ << (*defines_)[i].text << '\n';
        define_emitted_[i] = 1;
      }
    }
  }

  bcc::SourceManager& sm_;
  std::ostream& os_;
  unsigned line_ = 0;
  std::string file_;
  bool first_ = true;
  bool no_markers_;
  bool keep_comments_;

  // -dD recorded defines and which have been emitted.
  std::vector<RecordedDefine>* defines_ = nullptr;
  std::vector<char> define_emitted_;

  // -C per-file source scan cursor.
  bcc::FileID last_comment_fid_;
  std::size_t last_comment_end_ = 0;
};

}  // namespace

int main(int argc, char* argv[]) {
  Options opts = ParseArgs(argc, argv);

  bcc::FileManager fm;
  bcc::SourceManager sm(fm);
  bcc::TextDiagnosticPrinter printer(std::cerr);
  bcc::DiagnosticsEngine diag(&printer, &sm);
  bcc::HeaderSearch header_search(fm);
  for (const std::string& dir : opts.include_dirs) {
    header_search.AddAngledSearchPath(dir);
  }

  const bcc::FileEntry* entry =
      opts.input_path ? fm.GetFile(opts.input_path) : fm.GetStdin();
  if (!entry) {
    diag.Report(bcc::diag::err_no_input_file);
    return 1;
  }

  sm.SetMainFileID(sm.CreateFileID(*entry));

  bcc::Preprocessor pp(sm, diag, header_search);

  pp.EnterMainFile();

  // -dM: drain the TU to reach the final macro state, then print every active
  // #define (sorted by name) instead of normal output.
  if (opts.dump_macros) {
    for (;;) {
      bcc::Token tok = pp.Lex();
      if (tok.GetKind() == bcc::TokenKind::kEOF) break;
    }
    std::vector<std::pair<std::string, std::string>> defs;
    pp.ForEachDefinedMacro(
        [&](const bcc::IdentifierInfo* name, const bcc::MacroInfo* macro) {
          defs.emplace_back(std::string(name->GetName()),
                            bcc::FormatMacroDefine(*name, *macro));
        });
    std::sort(defs.begin(), defs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [_, text] : defs) std::cout << text << '\n';
    return diag.HasErrors() ? 1 : 0;
  }

  // -M / -MM: install the collector, drain the TU (includes fire during Lex),
  // then emit a Makefile dependency rule.
  if (opts.dep_makefile) {
    auto collector = std::make_unique<DepCollector>(opts.dep_user_only);
    DepCollector* deps = collector.get();
    pp.SetPPCallbacks(std::move(collector));
    for (;;) {
      bcc::Token tok = pp.Lex();
      if (tok.GetKind() == bcc::TokenKind::kEOF) break;
    }
    std::string target =
        opts.dep_target.empty() && opts.input_path
            ? DefaultTarget(opts.input_path)
            : (opts.dep_target.empty() ? std::string("a.o") : opts.dep_target);
    std::cout << target << ':';
    if (opts.input_path) std::cout << ' ' << opts.input_path;
    for (const std::string& d : deps->deps_) std::cout << ' ' << d;
    std::cout << '\n';
    return diag.HasErrors() ? 1 : 0;
  }

  // Default -E (with optional -P / -C / -dD). The -dD recorder is installed
  // before Lexing so #define events are captured for interleaving.
  std::vector<RecordedDefine> recorded_defines;
  if (opts.retain_defines) {
    pp.SetPPCallbacks(std::make_unique<DefineRecorder>(sm, recorded_defines));
  }

  TextPrinter out(sm, std::cout, opts.no_line_markers, opts.keep_comments,
                  opts.retain_defines ? &recorded_defines : nullptr);
  for (;;) {
    bcc::Token tok = pp.Lex();
    if (tok.GetKind() == bcc::TokenKind::kEOF) break;
    out.Emit(tok);
  }
  out.Finish();

  return diag.HasErrors() ? 1 : 0;
}
