#pragma once

#include <iosfwd>
#include <string>
#include <vector>

#include "bcc/basic/diagnostic.hh"
#include "bcc/basic/file_id.hh"

namespace bcc {

/// \brief Collects diagnostics in memory for later inspection.
class TextDiagnosticBuffer final : public DiagnosticConsumer {
 public:
  struct Entry {
    DiagSeverity severity;
    std::string message;  ///< Pre-formatted message text.
  };

  void HandleDiagnostic(const DiagnosticInfo& info) override;

  const std::vector<Entry>& Entries() const noexcept { return entries_; }

 private:
  std::vector<Entry> entries_;
};

/// \brief Renders diagnostics as human-readable text.
///
/// Output mirrors the GCC/Clang format:
/// \code
///   In file included from foo.h:10:
///   bar.c:5:3: error: some message
///     source_line_text
///     ^~~~~~~~~~~~~~~
/// \endcode
///
/// ANSI color is auto-detected from whether the output stream is a terminal,
/// but can be forced via UseColor().
class TextDiagnosticPrinter final : public DiagnosticConsumer {
 public:
  /// \param os  Output stream to write to (typically std::cerr).
  explicit TextDiagnosticPrinter(std::ostream& os);

  /// \brief Override the auto-detected color setting.
  void UseColor(bool enabled) noexcept { color_ = enabled; }

  void HandleDiagnostic(const DiagnosticInfo& info) override;

 private:
  void PrintIncludeStack(SourceLocation loc, const SourceManager& sm);
  void PrintHeader(const DiagnosticInfo& info, const SourceManager* sm);
  void PrintSourceSnippet(SourceLocation loc,
                          std::span<const SourceRange> ranges,
                          const SourceManager& sm);
  void PrintMacroNotes(SourceLocation loc, const SourceManager& sm);
  void PrintFixIts(std::span<const FixItHint> fix_its);

  std::string_view SeverityLabel(DiagSeverity sev) const noexcept;
  const char* SeverityColor(DiagSeverity sev) const noexcept;

  std::ostream& os_;
  bool color_;

  /// FileID of the last file for which we printed an include stack.
  FileID last_fid_;
};

}  // namespace bcc
