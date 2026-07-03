#pragma once

#include <ostream>
#include <string>
#include <string_view>

#include "bcc/as/lexer.hh"

namespace bcc::as {

/// \brief Minimal diagnostic sink for the assembler.
///
/// Formats messages as `file:line:col: error: message` using the lexer to map
/// a byte offset back to a line/column, and counts errors so the driver can
/// set its exit status. Assembly continues after recoverable errors so several
/// can be reported in one run.
class AsmDiag {
 public:
  AsmDiag(std::string filename, const Lexer& lexer, std::ostream& os)
      : filename_(std::move(filename)), lexer_(lexer), os_(os) {}

  void Error(uint32_t offset, std::string_view msg) {
    Report(offset, "error", msg);
    ++num_errors_;
  }
  void Warning(uint32_t offset, std::string_view msg) {
    Report(offset, "warning", msg);
  }

  unsigned num_errors() const noexcept { return num_errors_; }
  bool has_error() const noexcept { return num_errors_ > 0; }

 private:
  void Report(uint32_t offset, std::string_view level, std::string_view msg) {
    uint32_t line = 0, col = 0;
    lexer_.GetLineCol(offset, line, col);
    os_ << filename_ << ':' << line << ':' << col << ": " << level << ": "
        << msg << '\n';
  }

  std::string filename_;
  const Lexer& lexer_;
  std::ostream& os_;
  unsigned num_errors_ = 0;
};

}  // namespace bcc::as
