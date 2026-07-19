#include "bcc/lex/numeric_literal.hh"

#include <cctype>
#include <string>

namespace bcc {
namespace {

int DigitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

char Lower(char c) {
  return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

}  // namespace

NumericLiteralParser::NumericLiteralParser(std::string_view spelling)
    : spelling_(spelling) {
  if (spelling_.empty()) {
    SetError(Error::kMissingDigits);
    return;
  }

  std::size_t pos = 0;
  if (spelling_[0] == '0' && spelling_.size() >= 2) {
    char prefix = Lower(spelling_[1]);
    if (prefix == 'x') {
      radix_ = 16;
      pos = 2;
    } else if (prefix == 'b') {
      radix_ = 2;
      pos = 2;
    } else {
      radix_ = 8;
    }
  } else if (spelling_[0] == '.') {
    is_integer_ = false;
  }

  digits_begin_ = pos;
  bool leading_period = spelling_[0] == '.';
  bool had_integral_digits = false;
  if (!leading_period) {
    if (ParseDigits(pos)) {
      had_integral_digits = true;
    } else if (error_ == Error::kMissingDigits && pos < spelling_.size() &&
               spelling_[pos] == '.') {
      error_ = Error::kNone;
    } else {
      return;
    }
  }

  bool saw_period = false;
  if (pos < spelling_.size() && spelling_[pos] == '.') {
    saw_period = true;
    is_integer_ = false;
    ++pos;
    if (!ParseDigits(pos)) {
      if (error_ != Error::kMissingDigits) return;
      // A fractional part may be empty when the integral part was nonempty.
      error_ = Error::kNone;
      if (!had_integral_digits) {
        SetError(Error::kMissingDigits);
        return;
      }
    }
  }

  char exponent = radix_ == 16 ? 'p' : 'e';
  if (pos < spelling_.size() && Lower(spelling_[pos]) == exponent) {
    is_integer_ = false;
    if (!ParseExponent(pos, exponent)) return;
  } else if (radix_ == 16 && saw_period) {
    // C hexadecimal floating constants require a binary exponent.
    SetError(Error::kMissingExponentDigits);
    return;
  }

  float_end_ = pos;
  if (is_integer_)
    ParseIntegerSuffix(pos);
  else
    ParseFloatSuffix(pos);
}

bool NumericLiteralParser::ParseDigits(std::size_t& pos) {
  bool saw_digit = false;
  bool previous_separator = false;
  for (; pos < spelling_.size(); ++pos) {
    char c = spelling_[pos];
    if (c == '\'') {
      if (!saw_digit || previous_separator || pos + 1 == spelling_.size()) {
        SetError(Error::kInvalidSeparator);
        return false;
      }
      previous_separator = true;
      continue;
    }
    int digit = DigitValue(c);
    if (digit < 0) break;
    if (static_cast<unsigned>(digit) >= radix_) {
      // A decimal digit outside the radix is always invalid (for example, 8
      // in an octal literal). An alphabetic hexadecimal digit can instead be
      // the exponent marker or suffix and is validated by the next stage.
      if (c >= '0' && c <= '9') {
        SetError(Error::kInvalidDigit);
        return false;
      }
      break;
    }
    saw_digit = true;
    previous_separator = false;
  }
  if (previous_separator) {
    SetError(Error::kInvalidSeparator);
    return false;
  }
  if (!saw_digit) {
    SetError(Error::kMissingDigits);
    return false;
  }
  digits_end_ = pos;
  return true;
}

bool NumericLiteralParser::ParseExponent(std::size_t& pos, char) {
  ++pos;
  if (pos < spelling_.size() &&
      (spelling_[pos] == '+' || spelling_[pos] == '-'))
    ++pos;
  std::size_t begin = pos;
  bool previous_separator = false;
  for (; pos < spelling_.size(); ++pos) {
    char c = spelling_[pos];
    if (c == '\'') {
      if (pos == begin || previous_separator) {
        SetError(Error::kInvalidSeparator);
        return false;
      }
      previous_separator = true;
      continue;
    }
    if (c < '0' || c > '9') break;
    previous_separator = false;
  }
  if (pos == begin || previous_separator) {
    SetError(Error::kMissingExponentDigits);
    return false;
  }
  return true;
}

bool NumericLiteralParser::ParseIntegerSuffix(std::size_t pos) {
  bool saw_size = false;
  while (pos < spelling_.size()) {
    char c = Lower(spelling_[pos]);
    if (c == 'u') {
      if (is_unsigned_) {
        SetError(Error::kInvalidSuffix);
        return false;
      }
      is_unsigned_ = true;
      ++pos;
      continue;
    }
    if (c == 'l') {
      if (saw_size) {
        SetError(Error::kInvalidSuffix);
        return false;
      }
      saw_size = true;
      if (pos + 1 < spelling_.size() && spelling_[pos + 1] == spelling_[pos]) {
        is_long_long_ = true;
        pos += 2;
      } else {
        is_long_ = true;
        ++pos;
      }
      continue;
    }
    SetError(Error::kInvalidSuffix);
    return false;
  }
  return true;
}

bool NumericLiteralParser::ParseFloatSuffix(std::size_t pos) {
  if (pos == spelling_.size()) return true;
  char c = Lower(spelling_[pos]);
  if (pos + 1 != spelling_.size() || (c != 'f' && c != 'l')) {
    SetError(Error::kInvalidSuffix);
    return false;
  }
  is_float_ = c == 'f';
  is_long_ = c == 'l';
  return true;
}

bool NumericLiteralParser::GetIntegerValue(APSInt& value,
                                           unsigned bit_width) const {
  APInt result(bit_width, 0);
  APInt radix_value(bit_width, radix_);
  bool overflow = false;
  for (std::size_t pos = digits_begin_; pos < digits_end_; ++pos) {
    if (spelling_[pos] == '\'') continue;
    APInt old = result;
    result *= radix_value;
    overflow |= result.UDiv(radix_value) != old;
    APInt digit(bit_width, static_cast<uint64_t>(DigitValue(spelling_[pos])));
    result += digit;
    overflow |= result.ULt(digit);
  }
  value = APSInt(std::move(result), is_unsigned_);
  return overflow;
}

Expected<APFloat, NumericLiteralParser::Error>
NumericLiteralParser::GetFloatValue(RoundingMode rounding) const {
  if (HadError() || IsIntegerLiteral()) {
    return UnExpected(HadError() ? error_ : Error::kInvalidSuffix);
  }
  std::string cleaned;
  cleaned.reserve(float_end_);
  for (std::size_t pos = 0; pos < float_end_; ++pos)
    if (spelling_[pos] != '\'') cleaned.push_back(spelling_[pos]);
  FloatFormat format = is_float_  ? fmt::kFloat
                       : is_long_ ? fmt::kX87_80
                                  : fmt::kDouble;
  return APFloat(format, cleaned, nullptr, rounding);
}

Expected<APValue, NumericLiteralParser::Error> NumericLiteralParser::GetValue(
    unsigned integer_bit_width) const {
  if (HadError()) return UnExpected(error_);
  if (IsFloatingLiteral()) {
    auto value = GetFloatValue();
    if (!value) return UnExpected(value.Error());
    return APValue(std::move(value).Value());
  }

  APSInt value;
  if (GetIntegerValue(value, integer_bit_width)) {
    return UnExpected(Error::kOverflow);
  }
  return APValue(std::move(value));
}

}  // namespace bcc
