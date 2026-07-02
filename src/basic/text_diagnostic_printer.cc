#include "bcc/basic/text_diagnostic_printer.hh"

#include <unistd.h>

#include <algorithm>
#include <iostream>
#include <ostream>

#include "bcc/basic/presumed_loc.hh"
#include "bcc/basic/source_manager.hh"

namespace bcc {

namespace {

bool DetectColor(std::ostream& os) {
  if (&os == &std::cerr || &os == &std::clog) return isatty(STDERR_FILENO) != 0;
  if (&os == &std::cout) return isatty(STDOUT_FILENO) != 0;

  return false;
}

constexpr const char* kReset = "\033[0m";
constexpr const char* kBold = "\033[1m";
constexpr const char* kGreen = "\033[1;32m";

}  // namespace

void TextDiagnosticBuffer::HandleDiagnostic(const DiagnosticInfo& info) {
  entries_.push_back({info.GetSeverity(), info.FormatMessage()});
}

TextDiagnosticPrinter::TextDiagnosticPrinter(std::ostream& os)
    : os_(os), color_(DetectColor(os)) {}

std::string_view TextDiagnosticPrinter::SeverityLabel(
    DiagSeverity sev) const noexcept {
  switch (sev) {
    case DiagSeverity::kNote:
      return "note";
    case DiagSeverity::kWarning:
      return "warning";
    case DiagSeverity::kError:
      return "error";
    case DiagSeverity::kFatal:
      return "fatal error";
  }

  return "error";
}

const char* TextDiagnosticPrinter::SeverityColor(
    DiagSeverity sev) const noexcept {
  switch (sev) {
    case DiagSeverity::kNote:
      return "\033[1m";  // bold
    case DiagSeverity::kWarning:
      return "\033[1;35m";  // bold magenta
    case DiagSeverity::kError:
      return "\033[1;31m";  // bold red
    case DiagSeverity::kFatal:
      return "\033[1;31m";  // bold red
  }

  return "\033[1;31m";
}

// Walks the include chain for `loc` and prints "In file included from …"
// lines from outermost to innermost.
void TextDiagnosticPrinter::PrintIncludeStack(SourceLocation loc,
                                              const SourceManager& sm) {
  FileID fid = sm.GetFileID(sm.GetExpansionLoc(loc));

  // Collect the chain bottom-up: each element is the #include site that
  // brought the next file in.
  std::vector<PresumedLoc> chain;
  SourceLocation inc = sm.GetIncludeLoc(fid);

  while (inc.IsValid()) {
    chain.push_back(sm.GetPresumedLoc(inc));
    inc = sm.GetIncludeLoc(sm.GetFileID(inc));
  }

  // Print top-down (outermost first).
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    if (!it->IsValid()) continue;

    os_ << "In file included from ";

    if (color_) os_ << kBold;

    os_ << it->filename << ':' << it->line;

    if (color_) os_ << kReset;

    os_ << ":\n";
  }
}

// Prints "filename:line:col: severity: message\n".
void TextDiagnosticPrinter::PrintHeader(const DiagnosticInfo& info,
                                        const SourceManager* sm) {
  SourceLocation loc = info.GetLocation();

  if (sm && loc.IsValid()) {
    SourceLocation display_loc = sm->GetExpansionLoc(loc);
    PresumedLoc ploc = sm->GetPresumedLoc(display_loc);

    if (ploc.IsValid()) {
      if (color_) os_ << kBold;

      os_ << ploc.filename << ':' << ploc.line << ':' << ploc.col << ':';

      if (color_) os_ << kReset;

      os_ << ' ';
    }
  }

  DiagSeverity sev = info.GetSeverity();

  if (color_) os_ << SeverityColor(sev);

  os_ << SeverityLabel(sev) << ':';

  if (color_) os_ << kReset;

  os_ << ' ' << info.FormatMessage() << '\n';
}

// Prints the source line containing `loc` and a caret/tilde indicator line.
// `loc` must be a file location (not a macro expansion location).
void TextDiagnosticPrinter::PrintSourceSnippet(
    SourceLocation loc, std::span<const SourceRange> ranges,
    const SourceManager& sm) {
  if (!loc.IsValid()) return;

  auto [fid, offset] = sm.GetDecomposedLoc(loc);
  std::string_view buffer = sm.GetBufferData(fid);

  if (buffer.empty()) return;

  // Locate the line boundaries.
  size_t line_start = offset;
  while (line_start > 0 && buffer[line_start - 1] != '\n') --line_start;

  size_t line_end = offset;

  while (line_end < buffer.size() && buffer[line_end] != '\n' &&
         buffer[line_end] != '\r') {
    ++line_end;
  }

  std::string_view line = buffer.substr(line_start, line_end - line_start);

  os_ << "  " << line << '\n';

  // Build the indicator string (spaces, tildes, caret).
  std::string indicator(line.size(), ' ');

  // Mark source ranges with '~'.
  for (const SourceRange& r : ranges) {
    if (!r.begin.IsValid() || !r.end.IsValid()) continue;

    auto [bfid, boff] = sm.GetDecomposedLoc(sm.GetExpansionLoc(r.begin));
    auto [efid, eoff] = sm.GetDecomposedLoc(sm.GetExpansionLoc(r.end));

    if (bfid != fid || efid != fid) continue;
    if (boff < line_start || boff >= line_end) continue;

    unsigned bci = static_cast<unsigned>(boff - line_start);
    unsigned eci = static_cast<unsigned>(
        std::min(static_cast<size_t>(eoff), line_end) - line_start);

    for (unsigned i = bci; i < eci && i < indicator.size(); ++i) {
      indicator[i] = '~';
    }
  }

  // Primary caret overwrites any tilde at the token start.
  unsigned col = static_cast<unsigned>(offset - line_start);

  if (col < indicator.size()) {
    indicator[col] = '^';
  } else {
    indicator += '^';
  }

  // Trim trailing spaces for a cleaner look.
  size_t last = indicator.find_last_not_of(' ');

  if (last != std::string::npos) indicator.resize(last + 1);

  os_ << "  ";

  if (color_) os_ << kGreen;

  os_ << indicator;

  if (color_) os_ << kReset;

  os_ << '\n';
}

// Walks a macro expansion chain and emits a "note: expanded from …" line for
// each level until we reach a file location.
void TextDiagnosticPrinter::PrintMacroNotes(SourceLocation loc,
                                            const SourceManager& sm) {
  while (loc.IsMacroExpansion()) {
    SourceLocation spelling = sm.GetSpellingLoc(loc);
    PresumedLoc ploc = sm.GetPresumedLoc(spelling);

    if (ploc.IsValid()) {
      os_ << "  ";

      if (color_) os_ << "\033[1m";

      os_ << "note:";

      if (color_) os_ << kReset;

      os_ << " expanded from macro at ";

      if (color_) os_ << kBold;

      os_ << ploc.filename << ':' << ploc.line << ':' << ploc.col;

      if (color_) os_ << kReset;

      os_ << '\n';
    }

    // Move to the caller's site.
    loc = sm.GetExpansionLoc(loc);

    if (!loc.IsMacroExpansion()) break;
  }
}

// Prints fix-it hints as terse suggestion lines.
void TextDiagnosticPrinter::PrintFixIts(std::span<const FixItHint> fix_its) {
  for (const FixItHint& h : fix_its) {
    if (h.IsNull()) continue;

    os_ << "  ";

    if (color_) os_ << "\033[1m";

    os_ << "note:";

    if (color_) os_ << kReset;

    os_ << " suggestion: ";

    if (h.remove_range.begin == h.remove_range.end &&
        !h.code_to_insert.empty()) {
      os_ << "insert '" << h.code_to_insert << '\'';
    } else if (h.code_to_insert.empty()) {
      os_ << "remove this text";
    } else {
      os_ << "replace with '" << h.code_to_insert << '\'';
    }

    os_ << '\n';
  }
}

void TextDiagnosticPrinter::HandleDiagnostic(const DiagnosticInfo& info) {
  const SourceManager* sm = info.GetSourceManager();
  SourceLocation loc = info.GetLocation();

  if (sm && loc.IsValid()) {
    SourceLocation display_loc = sm->GetExpansionLoc(loc);
    FileID fid = sm->GetFileID(display_loc);

    // Print the include stack whenever we move into a different file.
    if (fid != last_fid_) {
      PrintIncludeStack(display_loc, *sm);
      last_fid_ = fid;
    }

    PrintHeader(info, sm);

    // Show the source snippet at the spelling location so the user sees the
    // actual token text, even when it came from a macro body.
    PrintSourceSnippet(sm->GetSpellingLoc(loc), info.GetRanges(), *sm);

    if (loc.IsMacroExpansion()) PrintMacroNotes(loc, *sm);
  } else {
    // No source location — just emit "severity: message".
    if (color_) os_ << SeverityColor(info.GetSeverity());

    os_ << SeverityLabel(info.GetSeverity()) << ':';

    if (color_) os_ << kReset;

    os_ << ' ' << info.FormatMessage() << '\n';
  }

  PrintFixIts(info.GetFixItHints());
}

}  // namespace bcc
