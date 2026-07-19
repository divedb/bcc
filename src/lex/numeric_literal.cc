#include "bcc/lex/numeric_literal.hh"

#include <cctype>
#include <string>

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringRef.h"

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
      // e/E begins a decimal exponent and is not an invalid octal digit.
      if (!(radix_ != 16 && Lower(c) == 'e')) {
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

bool NumericLiteralParser::GetIntegerValue(llvm::APSInt& value,
                                           unsigned bit_width) const {
  llvm::APInt result(bit_width, 0);
  llvm::APInt radix_value(bit_width, radix_);
  bool overflow = false;
  for (std::size_t pos = digits_begin_; pos < digits_end_; ++pos) {
    if (spelling_[pos] == '\'') continue;
    llvm::APInt old = result;
    result *= radix_value;
    overflow |= result.udiv(radix_value) != old;
    llvm::APInt digit(bit_width,
                      static_cast<uint64_t>(DigitValue(spelling_[pos])));
    result += digit;
    overflow |= result.ult(digit);
  }
  value = llvm::APSInt(std::move(result), is_unsigned_);
  return overflow;
}

llvm::Expected<llvm::APFloat::opStatus> NumericLiteralParser::GetFloatValue(
    llvm::APFloat& value, llvm::RoundingMode rounding) const {
  const llvm::fltSemantics& semantics = is_float_ ? llvm::APFloat::IEEEsingle()
                                        : is_long_
                                            ? llvm::APFloat::x87DoubleExtended()
                                            : llvm::APFloat::IEEEdouble();
  value = llvm::APFloat(semantics);
  std::string cleaned;
  cleaned.reserve(float_end_);
  for (std::size_t pos = 0; pos < float_end_; ++pos)
    if (spelling_[pos] != '\'') cleaned.push_back(spelling_[pos]);
  return value.convertFromString(llvm::StringRef(cleaned), rounding);
}

}  // namespace bcc
