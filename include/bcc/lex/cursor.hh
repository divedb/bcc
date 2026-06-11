#pragma once

#include <cassert>
#include <cstdint>
#include <string_view>

#include "bcc/common/string_util.hh"

namespace bcc {

struct DecodedChar {
  uint32_t codepoint;

  bool IsEOF() const noexcept { return codepoint == kEOF; }
  bool IsInvalidUTF8() const noexcept { return codepoint == kInvalid; }
  bool IsValid() const noexcept { return codepoint < kEOF; }

  static constexpr uint32_t kEOF = 0xFFFF'FFFE;
  static constexpr uint32_t kInvalid = 0xFFFF'FFFF;
};

class Cursor {
 public:
  explicit Cursor(std::string_view buffer) noexcept
      : begin_(buffer.data()),
        end_(buffer.data() + buffer.size()),
        current_(buffer.data()) {};

  /// \brief Decodes the next Unicode code point and advances the cursor.
  ///
  /// Always advances: by the byte length of the encoded character on success,
  /// by exactly one byte on invalid UTF-8. Does not advance on EOF.
  ///
  /// Any backslash-newline line splices are removed transparently before each
  /// byte is consumed. If one or more splices are skipped, HadLineSplice()
  /// becomes true and remains true until ResetLineSpliceFlag() is called.
  ///
  /// On invalid UTF-8 the cursor is placed exactly one byte past the lead
  /// byte of the rejected sequence, so the caller never needs a manual
  /// Advance() call to make progress.
  ///
  /// \return The decoded Unicode code point, kEOF, or kInvalid.
  DecodedChar Next() noexcept;

  void Advance(std::size_t n = 1) noexcept {
    assert(n <= Remaining());

    current_ += n;
  }

  bool AtEnd() const noexcept { return current_ >= end_; }

  const char* Begin() const noexcept { return begin_; }
  const char* End() const noexcept { return end_; }
  const char* Current() const noexcept { return current_; }

  /// \brief Reports whether a line splice has been skipped.
  ///
  /// \return true if any call to TryDecodeUTF8() since the last
  ///         ResetLineSpliceFlag() removed one or more backslash-newline
  ///         sequences. The flag remains set until explicitly cleared.
  bool HadLineSplice() const noexcept { return had_line_splice_; }

  void ResetLineSpliceFlag() noexcept { had_line_splice_ = false; }

 private:
  std::size_t Remaining() const noexcept {
    return static_cast<std::size_t>(end_ - current_);
  }

  bool IsLineSplice() const noexcept {
    return current_ + 1 < end_ && current_[0] == '\\' && IsNewLine(current_[1]);
  }

  /// \brief Skips consecutive C/C++ backslash-newline line splices.
  ///
  /// Removes one or more line splices starting at the current position.
  /// A line splice consists of a backslash immediately followed by a
  /// newline sequence:
  ///
  ///   "\\\n"
  ///   "\\\r"
  ///   "\\\r\n"
  ///
  /// For example, given:
  ///
  ///   "abc\\\n"
  ///   "def"
  ///
  /// the splice is removed and the logical character stream becomes:
  ///
  ///   "abcdef"
  ///
  /// Multiple adjacent line splices are skipped in a single call.
  ///
  /// \return True if at least one line splice was skipped.
  bool SkipLineSplice() noexcept {
    bool had_line_splice = false;

    while (IsLineSplice()) {
      ++current_;

      if (*current_ == '\r') {
        ++current_;

        if (current_ < end_ && *current_ == '\n') ++current_;
      } else {
        ++current_;
      }

      had_line_splice = true;
    }

    return had_line_splice;
  }

  const char* begin_;
  const char* end_;
  const char* current_;
  bool had_line_splice_ = false;
};

}  // namespace bcc
