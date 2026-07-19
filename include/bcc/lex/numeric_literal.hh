#pragma once

#include <string_view>

#include "bcc/support/apfloat.hh"
#include "bcc/support/apint.hh"
#include "bcc/support/apvalue.hh"
#include "bcc/support/expected.hh"

namespace bcc {

/// Strict semantic parser for a C preprocessing number.  Like Clang's
/// NumericLiteralParser, this is shared by the preprocessor and the eventual
/// semantic expression parser; the lexer itself intentionally only performs
/// the maximal-munch pp-number tokenization.
class NumericLiteralParser {
 public:
  enum class Error {
    kNone,
    kInvalidDigit,
    kInvalidSuffix,
    kMissingDigits,
    kMissingExponentDigits,
    kInvalidSeparator,
    kOverflow,
  };

  explicit NumericLiteralParser(std::string_view spelling);

  bool HadError() const noexcept { return error_ != Error::kNone; }
  Error GetError() const noexcept { return error_; }
  bool IsIntegerLiteral() const noexcept { return is_integer_; }
  bool IsFloatingLiteral() const noexcept {
    return !HadError() && !is_integer_;
  }
  bool IsUnsigned() const noexcept { return is_unsigned_; }
  bool IsLong() const noexcept { return is_long_; }
  bool IsLongLong() const noexcept { return is_long_long_; }
  unsigned GetRadix() const noexcept { return radix_; }

  /// Converts the digit sequence to an APSInt of the requested width. Returns
  /// true if unsigned magnitude overflow occurred. Signedness initially
  /// reflects the U suffix; callers apply context-specific type selection.
  bool GetIntegerValue(APSInt& value, unsigned bit_width) const;

  /// Converts a floating literal using the semantics selected by its suffix:
  /// f -> IEEE single, no suffix -> IEEE double, l -> x87 extended.
  /// Returns the APFloat conversion status.
  Expected<APFloat, Error> GetFloatValue(
      RoundingMode rounding = RoundingMode::kNearestEven) const;

  /// Parses and converts the literal to the common constant-value container.
  Expected<APValue, Error> GetValue(unsigned integer_bit_width = 64) const;

 private:
  bool ParseDigits(std::size_t& pos);
  bool ParseExponent(std::size_t& pos, char marker);
  bool ParseIntegerSuffix(std::size_t pos);
  bool ParseFloatSuffix(std::size_t pos);
  void SetError(Error error) noexcept {
    if (error_ == Error::kNone) error_ = error;
  }

  std::string_view spelling_;
  std::size_t digits_begin_ = 0;
  std::size_t digits_end_ = 0;
  std::size_t float_end_ = 0;
  unsigned radix_ = 10;
  Error error_ = Error::kNone;
  bool is_integer_ = true;
  bool is_unsigned_ = false;
  bool is_long_ = false;
  bool is_long_long_ = false;
  bool is_float_ = false;
};

}  // namespace bcc
