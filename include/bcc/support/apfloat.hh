#pragma once

#include <cfloat>
#include <climits>
#include <cstdlib>
#include <cstring>

#include "bcc/support/apint.hh"

namespace bcc {

/// \brief Enumeration of supported rounding modes for floating-point
/// operations.
///
/// EXAMPLE:
///  - RoundingMode::kNearestEven: Round to nearest, ties to even (IEEE default)
///    1.5 rounds to 2, 2.5 rounds to 2, -1.5 rounds to -2, -2.5 rounds to -2
///  - RoundingMode::kTowardZero: Round toward zero (truncate)
///    1.5 rounds to 1, 2.5 rounds to 2, -1.5 rounds to -1, -2.5 rounds to -2
///  - RoundingMode::kTowardPositive: Round toward positive infinity (ceil)
///    1.5 rounds to 2, 2.5 rounds to 3, -1.5 rounds to -1, -2.5 rounds to -2
///  - RoundingMode::kTowardNegative: Round toward negative infinity (floor)
///    1.5 rounds to 1, 2.5 rounds to 2, -1.5 rounds to -2, -2.5 rounds to -3
///  - RoundingMode::kNearestAway: Round to nearest, ties away from zero
///    1.5 rounds to 2, 2.5 rounds to 3, -1.5 rounds to -2, -2.5 rounds to -3
enum class RoundingMode : uint8_t {
  kNearestEven = 0,     ///< Round to nearest, ties to even   (IEEE default)
  kTowardZero = 1,      ///< Truncate
  kTowardPositive = 2,  ///< Ceil
  kTowardNegative = 3,  ///< Floor
  kNearestAway = 4,     ///< Round to nearest, ties away from zero
};

// ============================================================================
//  Status flags (IEEE 754-2008 §7)
// ============================================================================

/// \brief Status flags for floating-point operations, as defined in IEEE
///        754-2008 §7.
///
/// These flags indicate various conditions that can occur during floating-point
/// computations:
///   - inexact: The result was not exact (e.g., due to rounding).
///   - underflow: The result is too small to be represented in the normal
///     format.
///   - overflow: The result is too large to be represented in the normal
///     format.
///   - div_by_zero: A division by zero occurred.
///   - invalid_op: An invalid operation was performed (e.g., sqrt of a negative
///     number).
struct FloatStatus {
  bool inexact : 1 = false;
  bool underflow : 1 = false;
  bool overflow : 1 = false;
  bool div_by_zero : 1 = false;
  bool invalid_op : 1 = false;

  bool IsAnySet() const {
    return inexact || underflow || overflow || div_by_zero || invalid_op;
  }

  FloatStatus& operator|=(const FloatStatus& o) {
    inexact |= o.inexact;
    underflow |= o.underflow;
    overflow |= o.overflow;
    div_by_zero |= o.div_by_zero;
    invalid_op |= o.invalid_op;

    return *this;
  }
};

/// \brief Representation of a floating-point format, including its bit layout
///        and properties.
///
/// ┌─────────────── total_bits ───────────────┐
/// │                                          │
/// │   sign    exponent        mantissa       │
/// │    1      exp_bits       mant_bits       │
/// │   ─── ───────────── ───────────────────  │
/// │   [s] [eeeeeeee...] [mmmmmmmmmmmm....]   │
/// │                                          │
/// └──────────────────────────────────────────┘
struct FloatFormat {
  /// Total bits in the encoding.
  unsigned total_bits;

  /// Exponent field width.
  unsigned exp_bits;

  /// Mantissa (significand) field width (excl. hidden bit).
  unsigned mant_bits;

  /// true for x87 (explicit integer bit).
  // ┌──────────────────────────── 80 bits ───────────────────────────┐
  // │ sign (1) │ exponent (15) │ integer bit (1) │ fraction (63)     │
  // └────────────────────────────────────────────────────────────────┘
  /// IEEE (64 bits)
  // ┌────────────── 64 bits ─────────────┐
  // │ sign │ exponent │ fraction         │
  // │  1   │   11     │   52             │
  // └────────────────────────────────────┘
  bool has_int_bit;

  /// \brief Returns the total number of significant bits, including the integer
  ///        bit if present.
  ///
  /// \return The total number of significant bits in the format.
  constexpr unsigned SigBits() const { return mant_bits + 1; }

  /// \brief Returns the bias for the exponent, calculated as 2^(exp_bits - 1)
  ///        - 1.
  ///
  /// \return The bias for the exponent in the format.
  constexpr int64_t Bias() const { return (int64_t(1) << (exp_bits - 1)) - 1; }

  /// \brief Returns the maximum unbiased exponent value, calculated as the
  ///        bias.
  ///
  /// \return The maximum unbiased exponent value for the format.
  constexpr int64_t EMax() const { return Bias(); }

  /// \brief Returns the minimum unbiased exponent value, calculated as 1 -
  ///        bias.
  ///
  /// \return The minimum unbiased exponent value for the format.
  constexpr int64_t EMin() const { return 1 - Bias(); }

  /// \brief Returns the minimum unbiased exponent value for subnormal numbers,
  ///        calculated as EMin - SigBits() + 1.
  ///
  /// \return The minimum unbiased exponent value for subnormal numbers.
  constexpr int64_t EMinSub() const { return EMin() - int64_t(SigBits()); }

  constexpr uint64_t MaxBiasedExp() const {
    return (uint64_t(1) << exp_bits) - 1;
  }

  bool operator==(const FloatFormat&) const = default;
};

namespace fmt {

inline constexpr FloatFormat kHalf{16, 5, 10, false};
inline constexpr FloatFormat kFloat{32, 8, 23, false};
inline constexpr FloatFormat kDouble{64, 11, 52, false};
inline constexpr FloatFormat kQuad{128, 15, 112, false};
inline constexpr FloatFormat kX87_80{80, 15, 63, true};

}  // namespace fmt

enum class FloatCategory : uint8_t {
  kZero,
  kNormal,
  kSubnormal,
  kInfinity,
  kNaN,
};

class APFloat {
 public:
  /// \brief Get an APFloat representing zero for the specified format and sign.
  ///
  /// \param fmt      The floating-point format to use for this APFloat.
  /// \param negative Whether the zero should be negative.
  /// \return         An APFloat representing zero.
  static APFloat GetZero(FloatFormat fmt, bool negative = false) {
    APFloat r(fmt);
    r.sign_ = negative;

    return r;
  }

  /// \brief Get an APFloat representing infinity for the specified format and
  ///        sign.
  ///
  /// \param fmt      The floating-point format to use for this APFloat.
  /// \param negative Whether the infinity should be negative.
  /// \return         An APFloat representing infinity.
  static APFloat GetInf(FloatFormat fmt, bool negative = false) {
    APFloat r(fmt);
    r.sign_ = negative;
    r.category_ = FloatCategory::kInfinity;

    return r;
  }

  /// \brief Get an APFloat representing NaN for the specified format, sign, and
  ///        payload.
  ///
  /// \param fmt        The floating-point format to use for this APFloat.
  /// \param negative   Whether the NaN should be negative.
  /// \param signalling Whether the NaN should be signalling (true) or quiet
  ///                   (false).
  ///
  /// \param payload    The payload to encode in the NaN's significand
  ///                   (only the least significant bits are used).
  ///
  /// \return           An APFloat representing the specified NaN.
  static APFloat GetNaN(FloatFormat fmt, bool negative = false,
                        bool signalling = false, uint64_t payload = 0) {
    APFloat r(fmt);
    r.sign_ = negative;
    r.category_ = FloatCategory::kNaN;
    r.is_signalling_ = signalling;
    r.significand_ = APInt(fmt.SigBits(), 0u);

    if (!signalling) r.significand_.SetBit(fmt.mant_bits - 1);
    if (payload) r.significand_ |= APInt(fmt.SigBits(), payload);

    return r;
  }

  static APFloat GetLargest(FloatFormat fmt, bool negative = false);
  static APFloat GetSmallest(FloatFormat fmt, bool negative = false);
  static APFloat GetSmallestNormalized(FloatFormat fmt, bool negative = false);

  /// \brief Construct an APFloat with the specified format, initialized to
  ///        zero.
  ///
  /// \param fmt The floating-point format to use for this APFloat.
  explicit APFloat(FloatFormat fmt)
      : fmt_(fmt),
        sign_(false),
        exponent_(0),
        significand_(fmt.SigBits(), 0u),
        category_(FloatCategory::kZero) {}

  /// \brief Construct an APFloat with the specified format and value.
  ///
  /// \param fmt The floating-point format to use for this APFloat.
  /// \param v   The double value to initialize the APFloat with.
  APFloat(FloatFormat fmt, double v);

  /// \brief Construct an APFloat with the specified format and bit-pattern.
  ///
  /// \param fmt  The floating-point format to use for this APFloat.
  /// \param bits The APInt containing the bit-pattern to initialize the
  ///             APFloat.
  APFloat(FloatFormat fmt, const APInt& bits);

  /// \brief Construct an APFloat with the specified format and string
  ///        representation.
  ///
  /// \param fmt The floating-point format to use for this APFloat.
  /// \param str The string representation of the floating-point value.
  /// \param st  Optional pointer to a FloatStatus object to receive status
  ///            flags.
  ///
  /// \param rm  The rounding mode to use when converting the string to a
  ///            floating-point value.
  APFloat(FloatFormat fmt, std::string_view str, FloatStatus* st = nullptr,
          RoundingMode rm = RoundingMode::kNearestEven);

  FloatFormat GetFormat() const { return fmt_; }
  FloatCategory GetCategory() const { return category_; }
  bool IsZero() const { return category_ == FloatCategory::kZero; }
  bool IsNormal() const { return category_ == FloatCategory::kNormal; }
  bool IsSubnormal() const { return category_ == FloatCategory::kSubnormal; }
  bool IsInfinity() const { return category_ == FloatCategory::kInfinity; }
  bool IsNaN() const { return category_ == FloatCategory::kNaN; }
  bool IsFinite() const { return !IsInfinity() && !IsNaN(); }
  bool IsNegative() const { return sign_; }
  bool IsSignalling() const { return IsNaN() && is_signalling_; }
  bool IsQuiet() const { return IsNaN() && !is_signalling_; }

  APFloat Add(const APFloat& rhs, FloatStatus* st = nullptr,
              RoundingMode rm = RoundingMode::kNearestEven) const;
  APFloat Sub(const APFloat& rhs, FloatStatus* st = nullptr,
              RoundingMode rm = RoundingMode::kNearestEven) const;
  APFloat Mul(const APFloat& rhs, FloatStatus* st = nullptr,
              RoundingMode rm = RoundingMode::kNearestEven) const;
  APFloat Div(const APFloat& rhs, FloatStatus* st = nullptr,
              RoundingMode rm = RoundingMode::kNearestEven) const;
  APFloat Mod(const APFloat& rhs,
              FloatStatus* st = nullptr) const;  // IEEE remainder

  APFloat Neg() const {
    APFloat r = *this;
    r.sign_ = !r.sign_;

    return r;
  }

  APFloat Abs() const {
    APFloat r = *this;
    r.sign_ = false;

    return r;
  }

  APFloat Sqrt(FloatStatus* st = nullptr,
               RoundingMode rm = RoundingMode::kNearestEven) const;

  APFloat FusedMulAdd(const APFloat& mul, const APFloat& add,
                      FloatStatus* st = nullptr,
                      RoundingMode rm = RoundingMode::kNearestEven) const;

  enum class CmpResult { kLess, kEqual, kGreater, kUnordered };

  CmpResult Compare(const APFloat& rhs) const;

  bool operator==(const APFloat& o) const {
    return Compare(o) == CmpResult::kEqual;
  }

  bool operator<(const APFloat& o) const {
    return Compare(o) == CmpResult::kLess;
  }

  bool operator>(const APFloat& o) const {
    return Compare(o) == CmpResult::kGreater;
  }

  bool operator<=(const APFloat& o) const {
    auto c = Compare(o);
    return c == CmpResult::kLess || c == CmpResult::kEqual;
  }

  bool operator>=(const APFloat& o) const {
    auto c = Compare(o);
    return c == CmpResult::kGreater || c == CmpResult::kEqual;
  }

  /// \brief Convert this APFloat to another format, applying the specified
  ///        rounding mode.
  ///
  /// \param dst_fmt The destination floating-point format to convert to.
  /// \param st      Optional pointer to a FloatStatus object to receive status
  ///                flags.
  ///
  /// \param rm      The rounding mode to use for the conversion.
  /// \return        A new APFloat representing the converted value in the
  ///                destination format.
  APFloat Convert(FloatFormat dst_fmt, FloatStatus* st = nullptr,
                  RoundingMode rm = RoundingMode::kNearestEven) const;

  /// \brief Convert this APFloat to an APInt representing an integer value,
  ///        applying the specified rounding mode.
  ///
  /// \param int_bits  The bit width of the destination integer type.
  /// \param is_signed Whether the destination integer type is signed.
  /// \param st        Optional pointer to a FloatStatus object to receive
  ///                  status flags.
  ///
  /// \param rm        The rounding mode to use for the conversion.
  APInt ConvertToInteger(unsigned int_bits, bool is_signed,
                         FloatStatus* st = nullptr,
                         RoundingMode rm = RoundingMode::kNearestEven) const;

  /// \brief Convert an APInt to an APFloat with the specified format, treating
  ///        the APInt as either signed or unsigned, and applying the specified
  ///        rounding mode if necessary.
  ///
  /// \param fmt       The floating-point format to use for the resulting
  ///                  APFloat.
  ///
  /// \param val       The APInt value to convert to an APFloat.
  /// \param is_signed Whether to treat the APInt as signed (true) or unsigned
  ///                  (false) during conversion.
  ///
  /// \param st        Optional pointer to a FloatStatus object to
  ///                  receive status flags.
  ///
  /// \param rm        The rounding mode to use for the conversion (if needed).
  /// \return          An APFloat representing the converted value from the
  ///                  APInt.
  static APFloat ConvertFromAPInt(FloatFormat fmt, const APInt& val,
                                  bool is_signed, FloatStatus* st = nullptr,
                                  RoundingMode rm = RoundingMode::kNearestEven);

  /// \brief Convert this APFloat to a double-precision floating-point value,
  ///        applying the specified rounding mode if necessary.
  ///
  /// \return The double-precision floating-point representation of this
  ///         APFloat.
  double ToDouble() const;

  /// \brief Bitcast this APFloat to an APInt.
  ///
  /// \return An APInt representing the bit pattern of this APFloat.
  APInt BitcastToAPInt() const;

  /// \brief Produce a decimal string with enough digits to round-trip.
  ///
  /// \param format_precision   The precision to use for formatting.
  /// \param format_max_padding The maximum padding to use for formatting.
  /// \return                   A string representing the decimal value of this
  ///                           APFloat.
  std::string ToString(unsigned format_precision = 0,
                       unsigned format_max_padding = 3) const;

  /// \brief Produce a hexadecimal string representation of this APFloat.
  ///
  /// \return A string representing the hexadecimal value of this APFloat.
  std::string ToHexString() const;

  bool GetSign() const { return sign_; }
  int64_t GetExponent() const { return exponent_; }
  APInt GetSignificand() const { return significand_; }

 private:
  static APFloat MakeQNaN(FloatFormat fmt) {
    return GetNaN(fmt, false, false, 0);
  }

  // Add two finite normal/subnormal values (signs already handled by caller).
  static APFloat AddFinite(APFloat lhs, APFloat rhs, bool result_sign,
                           FloatStatus* st, RoundingMode rm);

  // Shift significand right by n bits, accumulating sticky.
  static bool ShiftRightSticky(APInt& sig, unsigned n);

  // Make exponent and significand of lhs and rhs comparable (align to same
  // exponent). Returns sticky bit from the alignment shift.
  static bool AlignSignificands(APFloat& lhs, APFloat& rhs);

  // Helper: construct from parts
  static APFloat FromParts(FloatFormat fmt, bool sign, int64_t exp, APInt sig,
                           FloatCategory cat = FloatCategory::kNormal) {
    APFloat r(fmt);
    r.sign_ = sign;
    r.exponent_ = exp;
    r.significand_ = std::move(sig);
    r.category_ = cat;

    return r;
  }

  static APFloat MakeInf(FloatFormat fmt, bool neg) { return GetInf(fmt, neg); }

  unsigned SigBits() const { return fmt_.SigBits(); }

  // Normalise: shift significand so MSB is at SigBits()-1, adjust exponent.
  // Returns true if value became zero (underflow to zero).
  bool Normalise();

  // Round significand to SigBits() after extended arithmetic.
  // 'guard', 'round', 'sticky' are the three trailing bits used by IEEE
  // rounding.
  void RoundToNearest(bool lsb, bool guard, bool round, bool sticky,
                      RoundingMode rm, FloatStatus* st);

  FloatFormat fmt_;
  bool sign_ = false;
  int64_t exponent_ = 0;
  APInt significand_;
  FloatCategory category_ = FloatCategory::kZero;
  bool is_signalling_ = false;
};

inline APInt APFloat::BitcastToAPInt() const {
  unsigned total = fmt_.total_bits;
  APInt result(total, 0u);
  unsigned exponent_offset =
      fmt_.has_int_bit ? fmt_.mant_bits + 1 : fmt_.mant_bits;

  if (IsNaN() || IsInfinity() || IsNormal() || IsSubnormal() || IsZero()) {
    if (sign_) result.SetBit(total - 1);

    uint64_t biased_exp = 0;

    if (IsNaN() || IsInfinity()) {
      biased_exp = fmt_.MaxBiasedExp();
    } else if (IsNormal()) {
      biased_exp = static_cast<uint64_t>(exponent_ + fmt_.Bias());
    } else {
      biased_exp = 0;  // zero or subnormal
    }

    for (unsigned i = 0; i < fmt_.exp_bits; ++i)
      if ((biased_exp >> i) & 1) result.SetBit(exponent_offset + i);

    // Pack mantissa
    // significand_ has integer bit at SigBits()-1; mantissa field excludes
    // integer bit (except x87 which stores it explicitly)
    APInt sig = significand_;

    if (fmt_.has_int_bit) {
      // x87: bits [62:0] = mantissa, bit 63 = integer bit, all in mantissa
      // field
      for (unsigned i = 0; i < fmt_.SigBits() && i < fmt_.mant_bits + 1; ++i)
        if (sig.GetBit(i)) result.SetBit(i);
    } else {
      // IEEE: hidden bit; copy mantBits from sig[mantBits-1:0]
      if (IsNaN()) {
        // Copy all sig bits (payload + quiet bit)
        for (unsigned i = 0; i < fmt_.mant_bits; ++i)
          if (sig.GetBit(i)) result.SetBit(i);
      } else if (IsNormal()) {
        // sig bit [SigBits()-1] is hidden; copy [SigBits()-2 : 0]
        for (unsigned i = 0; i < fmt_.mant_bits; ++i)
          if (sig.GetBit(i)) result.SetBit(i);
      } else {
        // Subnormal / zero: sig has no hidden bit set
        for (unsigned i = 0; i < fmt_.mant_bits; ++i)
          if (sig.GetBit(i)) result.SetBit(i);
      }
    }
  }
  return result;
}

inline APFloat::APFloat(FloatFormat fmt, const APInt& bits)
    : fmt_(fmt), significand_(fmt.SigBits(), 0u) {
  assert(bits.GetBitWidth() == fmt.total_bits);

  unsigned total = fmt.total_bits;
  unsigned exponent_offset =
      fmt.has_int_bit ? fmt.mant_bits + 1 : fmt.mant_bits;

  // Unpack sign
  sign_ = bits.GetBit(total - 1);

  // Unpack biased exponent
  uint64_t biased_exp = 0;
  for (unsigned i = 0; i < fmt.exp_bits; ++i)
    if (bits.GetBit(exponent_offset + i)) biased_exp |= uint64_t(1) << i;

  // Unpack mantissa bits
  APInt mantissa(fmt.SigBits(), 0u);
  unsigned mantField = fmt.has_int_bit ? fmt.mant_bits + 1 : fmt.mant_bits;
  for (unsigned i = 0; i < mantField && i < fmt.SigBits(); ++i)
    if (bits.GetBit(i)) mantissa.SetBit(i);

  uint64_t maxExp = fmt.MaxBiasedExp();

  if (biased_exp == maxExp) {
    // NaN or Infinity
    bool mantZero = mantissa.IsZero();
    if (mantZero) {
      category_ = FloatCategory::kInfinity;
    } else {
      category_ = FloatCategory::kNaN;
      // Quiet bit: MSB of mantissa field
      is_signalling_ = !mantissa.GetBit(fmt.mant_bits - 1);
      significand_ = mantissa;
    }
    exponent_ = 0;
  } else if (biased_exp == 0) {
    if (mantissa.IsZero()) {
      category_ = FloatCategory::kZero;
      exponent_ = 0;
    } else {
      // Subnormal
      category_ = FloatCategory::kSubnormal;
      exponent_ = fmt.EMin();
      significand_ = mantissa;  // no hidden bit
    }
  } else {
    // Normal
    category_ = FloatCategory::kNormal;
    exponent_ = static_cast<int64_t>(biased_exp) - fmt.Bias();
    // Set integer/hidden bit
    significand_ = mantissa;
    significand_.SetBit(fmt.SigBits() - 1);
  }
}

inline APFloat::APFloat(FloatFormat fmt, double v) : APFloat(fmt) {
  uint64_t bits;

  static_assert(sizeof(double) == 8);

  std::memcpy(&bits, &v, 8);
  APFloat tmp(fmt::kDouble, APInt(64, bits));
  *this = tmp.Convert(fmt);
}

inline double APFloat::ToDouble() const {
  APFloat d = Convert(fmt::kDouble);
  APInt bits = d.BitcastToAPInt();
  uint64_t raw = bits.GetZExtValue();
  double v;
  std::memcpy(&v, &raw, 8);

  return v;
}

inline APFloat APFloat::GetLargest(FloatFormat fmt, bool negative) {
  APFloat r(fmt);
  r.sign_ = negative;
  r.category_ = FloatCategory::kNormal;
  r.exponent_ = fmt.EMax();
  r.significand_ = APInt(fmt.SigBits(), 0u);
  r.significand_.SetAllBits();  // all 1s

  return r;
}

inline APFloat APFloat::GetSmallest(FloatFormat fmt, bool negative) {
  // Smallest positive (subnormal): 0...01
  APFloat r(fmt);
  r.sign_ = negative;
  r.category_ = FloatCategory::kSubnormal;
  r.exponent_ = fmt.EMin();
  r.significand_ = APInt(fmt.SigBits(), 1u);

  return r;
}

inline APFloat APFloat::GetSmallestNormalized(FloatFormat fmt, bool negative) {
  APFloat r(fmt);
  r.sign_ = negative;
  r.category_ = FloatCategory::kNormal;
  r.exponent_ = fmt.EMin();
  r.significand_ = APInt(fmt.SigBits(), 0u);
  r.significand_.SetBit(fmt.SigBits() - 1);

  return r;
}

/// Shift sig right by n, returning true if any shifted-out bits were non-zero
/// (sticky).
inline bool APFloat::ShiftRightSticky(APInt& sig, unsigned n) {
  if (n == 0) return false;

  if (n >= sig.GetBitWidth()) {
    bool sticky = !sig.IsZero();
    sig.ClearAllBits();
    return sticky;
  }
  // Collect sticky: any of the n LSBs
  APInt mask = APInt::GetAllOnes(n).ZExt(sig.GetBitWidth());
  bool sticky = !(sig & mask).IsZero();
  sig = sig.LShr(n);

  return sticky;
}

/// Align two significands to the same exponent (modify the one with smaller
/// exponent). Returns sticky bit from shifts.
inline bool APFloat::AlignSignificands(APFloat& lhs, APFloat& rhs) {
  if (lhs.exponent_ == rhs.exponent_) return false;
  // Make lhs the one with larger exponent
  if (lhs.exponent_ < rhs.exponent_) std::swap(lhs, rhs);
  int64_t diff = lhs.exponent_ - rhs.exponent_;
  bool sticky = ShiftRightSticky(
      rhs.significand_, static_cast<unsigned>(std::min(
                            diff, static_cast<int64_t>(rhs.SigBits() + 1))));
  rhs.exponent_ = lhs.exponent_;

  return sticky;
}

/// Normalise: shift significand MSB to SigBits()-1, adjust exponent.
inline bool APFloat::Normalise() {
  if (category_ == FloatCategory::kZero) return false;
  unsigned sb = SigBits();

  // Find actual MSB
  unsigned msb = 0;
  bool found = false;
  for (int i = static_cast<int>(sb) - 1; i >= 0; --i) {
    if (significand_.GetBit(i)) {
      msb = i;
      found = true;
      break;
    }
  }
  if (!found) {
    // Significand is zero → result is zero
    category_ = FloatCategory::kZero;
    exponent_ = 0;
    sign_ = false;  // preserve sign handled by caller
    significand_.ClearAllBits();
    return true;
  }

  int shift = static_cast<int>(sb) - 1 - static_cast<int>(msb);
  if (shift > 0) {
    // Shift left: exponent decreases
    significand_ = significand_.Shl(static_cast<unsigned>(shift));
    exponent_ -= shift;
  } else if (shift < 0) {
    // Shift right (overflow word): exponent increases
    ShiftRightSticky(significand_, static_cast<unsigned>(-shift));
    exponent_ -= shift;  // shift is negative, so this adds
  }

  // Check normal vs subnormal
  if (exponent_ < fmt_.EMin()) {
    category_ = FloatCategory::kSubnormal;
  } else {
    category_ = FloatCategory::kNormal;
  }
  return false;
}

/// Apply IEEE rounding to the current significand given guard/round/sticky
/// bits.
inline void APFloat::RoundToNearest(bool lsb, bool guard, bool round,
                                    bool sticky, RoundingMode rm,
                                    FloatStatus* st) {
  bool any_remainder = guard || round || sticky;
  if (!any_remainder) return;
  if (st) st->inexact = true;

  bool increment = false;
  switch (rm) {
    case RoundingMode::kNearestEven:
      // Round up if > 0.5 ulp, or exactly 0.5 ulp and lsb is 1
      increment = guard && (round || sticky || lsb);
      break;
    case RoundingMode::kNearestAway:
      increment = guard;
      break;
    case RoundingMode::kTowardZero:
      increment = false;
      break;
    case RoundingMode::kTowardPositive:
      increment = !sign_ && any_remainder;
      break;
    case RoundingMode::kTowardNegative:
      increment = sign_ && any_remainder;
      break;
  }

  if (increment) {
    // Add 1 ULP
    significand_ += APInt(SigBits(), 1u);
    // Check for carry out (significand overflow)
    if (significand_.GetBit(SigBits())) {
      // e.g. 1.111 + 1 ulp = 10.000; shift right
      ShiftRightSticky(significand_, 1);
      ++exponent_;
    }
  }
}

inline APFloat APFloat::AddFinite(APFloat lhs, APFloat rhs, bool result_sign,
                                  FloatStatus* st, RoundingMode rm) {
  // Both lhs, rhs are finite (normal or subnormal), same format.
  // Signs already disambiguated by caller; this is magnitude addition.
  FloatFormat fmt = lhs.fmt_;
  unsigned sb = fmt.SigBits();

  // Work in sb+2 bits to capture guard+sticky
  unsigned wb = sb + 2;
  APInt lhsSig = lhs.significand_.ZExt(wb);
  APInt rhsSig = rhs.significand_.ZExt(wb);

  // Align exponents
  bool sticky = false;
  if (lhs.exponent_ != rhs.exponent_) {
    if (lhs.exponent_ < rhs.exponent_) {
      std::swap(lhsSig, rhsSig);
      std::swap(lhs.exponent_, rhs.exponent_);
    }

    int64_t diff = lhs.exponent_ - rhs.exponent_;
    // Shift rhs right
    unsigned shiftAmt =
        static_cast<unsigned>(std::min(diff, static_cast<int64_t>(wb)));
    // Sticky = any bits shifted out
    if (shiftAmt >= wb) {
      sticky = !rhsSig.IsZero();
      rhsSig.ClearAllBits();
    } else {
      APInt stickMask = APInt::GetAllOnes(shiftAmt).ZExt(wb);
      sticky = !(rhsSig & stickMask).IsZero();
      rhsSig = rhsSig.LShr(shiftAmt);
    }
  }

  APInt sum = lhsSig + rhsSig;

  // Result exponent = lhs.exponent_ (the larger one)
  int64_t exp = lhs.exponent_;

  // Normalise: find MSB
  unsigned msb = 0;
  for (int i = static_cast<int>(wb) - 1; i >= 0; --i)
    if (sum.GetBit(i)) {
      msb = i;
      break;
    }

  // We need msb at position sb-1
  int shift = static_cast<int>(msb) - static_cast<int>(sb - 1);
  bool guard = false, round_bit = false;

  if (shift > 0) {
    // Carry: shift right
    if (shift >= 1) guard = sum.GetBit(0);
    if (shift >= 2) round_bit = sum.GetBit(1);
    // Accumulate sticky from already-sticky + bits shifted out
    APInt stickMask =
        (shift >= 2)
            ? APInt::GetAllOnes(static_cast<unsigned>(shift - 1)).ZExt(wb)
            : APInt(wb, 0u);
    sticky |= !(sum & stickMask).IsZero();
    sum = sum.LShr(static_cast<unsigned>(shift));
    exp += shift;
  } else if (shift < 0) {
    // Need to shift left (e.g. after cancellation)
    sum = sum.Shl(static_cast<unsigned>(-shift));
    exp -= (-shift);
  }

  // Truncate to sb bits
  APInt sig = sum.Trunc(sb);

  APFloat result(fmt);
  result.sign_ = result_sign;
  result.exponent_ = exp;
  result.significand_ = sig;
  result.category_ = FloatCategory::kNormal;

  // Round
  bool lsb = sig.GetBit(0);
  result.RoundToNearest(lsb, guard, round_bit, sticky, rm, st);

  // Re-normalise after rounding
  result.Normalise();

  // Overflow check
  if (result.exponent_ > fmt.EMax()) {
    if (st) {
      st->overflow = true;
      st->inexact = true;
    }
    // Round to infinity or max depending on rounding mode
    bool to_inf = true;
    if (rm == RoundingMode::kTowardZero) to_inf = false;
    if (rm == RoundingMode::kTowardPositive && result_sign) to_inf = false;
    if (rm == RoundingMode::kTowardNegative && !result_sign) to_inf = false;
    return to_inf ? GetInf(fmt, result_sign) : GetLargest(fmt, result_sign);
  }

  // Underflow check
  if (result.exponent_ < fmt.EMin() && !result.IsZero()) {
    if (st) st->underflow = true;
    result.category_ = FloatCategory::kSubnormal;
  }

  return result;
}

inline APFloat APFloat::Add(const APFloat& rhs, FloatStatus* st,
                            RoundingMode rm) const {
  FloatFormat f = fmt_;
  assert(f == rhs.fmt_ && "APFloat format mismatch");

  // Handle NaN
  if (IsNaN() || rhs.IsNaN()) {
    if (st) st->invalid_op = true;

    return MakeQNaN(f);
  }

  // Inf + Inf (same sign) = Inf; Inf + (-Inf) = NaN
  if (IsInfinity() && rhs.IsInfinity()) {
    if (sign_ == rhs.sign_) return *this;
    if (st) st->invalid_op = true;

    return MakeQNaN(f);
  }
  if (IsInfinity()) return *this;
  if (rhs.IsInfinity()) return rhs;

  // (+0) + (-0) = +0 (for NearestEven); otherwise preserve sign
  if (IsZero() && rhs.IsZero()) {
    bool s = (rm == RoundingMode::kTowardNegative) ? (sign_ || rhs.sign_)
                                                   : (sign_ && rhs.sign_);
    return GetZero(f, s);
  }
  if (IsZero()) return rhs;
  if (rhs.IsZero()) return *this;

  // Both finite non-zero
  bool effectiveSubtract = (sign_ != rhs.sign_);

  if (!effectiveSubtract) {
    // Same sign: add magnitudes
    return AddFinite(*this, rhs, sign_, st, rm);
  } else {
    // Different signs: subtract magnitudes
    // Determine whose magnitude is larger
    APFloat a = this->Abs(), b = rhs.Abs();
    CmpResult cmp = a.Compare(b);
    if (cmp == CmpResult::kEqual)
      return GetZero(f, rm == RoundingMode::kTowardNegative);

    bool result_sign;
    APFloat larger(f), smaller(f);
    if (cmp == CmpResult::kGreater) {
      result_sign = sign_;
      larger = *this;
      smaller = rhs;
    } else {
      result_sign = rhs.sign_;
      larger = rhs;
      smaller = *this;
    }

    // Subtract: larger - smaller (magnitudes)
    unsigned sb = f.SigBits();
    unsigned wb = sb + 2;
    APInt lsig = larger.significand_.ZExt(wb);
    APInt sSig = smaller.significand_.ZExt(wb);

    // Align
    int64_t diff = larger.exponent_ - smaller.exponent_;
    if (diff > 0) {
      unsigned shiftAmt =
          static_cast<unsigned>(std::min(diff, static_cast<int64_t>(wb)));
      if (shiftAmt >= wb)
        sSig.ClearAllBits();
      else
        sSig = sSig.LShr(shiftAmt);
    }

    // lsig >= sSig at this point
    APInt res = lsig - sSig;
    int64_t exp = larger.exponent_;

    // Normalise: find MSB
    int msb = -1;
    for (int i = static_cast<int>(wb) - 1; i >= 0; --i)
      if (res.GetBit(i)) {
        msb = i;
        break;
      }

    if (msb < 0) return GetZero(f, rm == RoundingMode::kTowardNegative);

    int shift = static_cast<int>(sb - 1) - msb;
    if (shift > 0) {
      res = res.Shl(static_cast<unsigned>(shift));
      exp -= shift;
    }

    APInt sig = res.Trunc(sb);
    APFloat result = FromParts(f, result_sign, exp, sig);
    result.Normalise();

    if (result.exponent_ < f.EMin() && !result.IsZero()) {
      if (st) st->underflow = true;

      result.category_ = FloatCategory::kSubnormal;
    }

    return result;
  }
}

inline APFloat APFloat::Sub(const APFloat& rhs, FloatStatus* st,
                            RoundingMode rm) const {
  return Add(rhs.Neg(), st, rm);
}

inline APFloat APFloat::Mul(const APFloat& rhs, FloatStatus* st,
                            RoundingMode rm) const {
  FloatFormat f = fmt_;
  assert(f == rhs.fmt_);

  bool result_sign = sign_ ^ rhs.sign_;

  // NaN
  if (IsNaN() || rhs.IsNaN()) {
    if (st) st->invalid_op = true;

    return MakeQNaN(f);
  }

  // Inf * 0 = NaN
  if ((IsInfinity() && rhs.IsZero()) || (IsZero() && rhs.IsInfinity())) {
    if (st) st->invalid_op = true;
    return MakeQNaN(f);
  }

  if (IsInfinity() || rhs.IsInfinity()) return GetInf(f, result_sign);
  if (IsZero() || rhs.IsZero()) return GetZero(f, result_sign);

  // Both finite non-zero
  unsigned sb = f.SigBits();
  unsigned wb = sb * 2;  // full product width
  APInt lsig = significand_.ZExt(wb);
  APInt rsig = rhs.significand_.ZExt(wb);
  APInt prod = lsig * rsig;

  int64_t exp = exponent_ + rhs.exponent_ - static_cast<int64_t>(sb - 1);

  // Product significand has integer bits at positions [2*(sb-1) .. 2*(sb-1)+1].
  // We need the MSB at position sb-1.
  // Find actual MSB of product.
  unsigned msb = 0;
  bool prod_non_zero = false;
  for (int i = static_cast<int>(wb) - 1; i >= 0; --i) {
    if (prod.GetBit(i)) {
      msb = i;
      prod_non_zero = true;
      break;
    }
  }
  if (!prod_non_zero) return GetZero(f, result_sign);

  unsigned target_msb = sb - 1;
  bool guard = false, round_bit = false;
  bool sticky = false;

  if (msb >= target_msb) {
    unsigned shift = msb - target_msb;
    exp += static_cast<int64_t>(shift);
    // Collect rounding bits from the shifted-out portion
    if (shift >= 1) guard = prod.GetBit(shift - 1);
    if (shift >= 2) round_bit = prod.GetBit(shift - 2);
    if (shift >= 3) {
      APInt sm = APInt::GetAllOnes(shift - 2).ZExt(wb);
      sticky = !(prod & sm).IsZero();
    }
    prod = prod.LShr(shift);
  } else {
    // msb < target_msb: shift left
    unsigned shift = target_msb - msb;
    prod = prod.Shl(shift);
    exp -= static_cast<int64_t>(shift);
  }

  APInt sig = prod.Trunc(sb);
  APFloat result = FromParts(f, result_sign, exp, sig);
  bool lsb = sig.GetBit(0);
  result.RoundToNearest(lsb, guard, round_bit, sticky, rm, st);
  result.Normalise();

  if (result.exponent_ > f.EMax()) {
    if (st) {
      st->overflow = true;
      st->inexact = true;
    }

    return (rm == RoundingMode::kTowardZero ||
            (rm == RoundingMode::kTowardPositive && result_sign) ||
            (rm == RoundingMode::kTowardNegative && !result_sign))
               ? GetLargest(f, result_sign)
               : GetInf(f, result_sign);
  }

  if (result.exponent_ < f.EMin() && !result.IsZero()) {
    if (st) st->underflow = true;

    result.category_ = FloatCategory::kSubnormal;
  }

  return result;
}

inline APFloat APFloat::Div(const APFloat& rhs, FloatStatus* st,
                            RoundingMode rm) const {
  FloatFormat f = fmt_;

  assert(f == rhs.fmt_);

  bool result_sign = sign_ ^ rhs.sign_;

  if (IsNaN() || rhs.IsNaN()) {
    if (st) st->invalid_op = true;

    return MakeQNaN(f);
  }

  if (IsInfinity() && rhs.IsInfinity()) {
    if (st) st->invalid_op = true;

    return MakeQNaN(f);
  }

  if (IsZero() && rhs.IsZero()) {
    if (st) st->invalid_op = true;

    return MakeQNaN(f);
  }

  if (IsInfinity()) return GetInf(f, result_sign);

  if (rhs.IsZero()) {
    if (st) st->div_by_zero = true;
    return GetInf(f, result_sign);
  }

  if (IsZero()) return GetZero(f, result_sign);
  if (rhs.IsInfinity()) return GetZero(f, result_sign);

  unsigned sb = f.SigBits();
  // Extend lhs significand by sb bits for precision
  unsigned wb = sb * 2 + 2;
  APInt lsig =
      significand_.ZExt(wb).Shl(sb);  // shift left sb for integer division
  APInt rsig = rhs.significand_.ZExt(wb);

  APInt quot = lsig.UDiv(rsig);
  APInt rem = lsig.URem(rsig);
  bool sticky = !rem.IsZero();

  // `quot` is computed from `(lhs_sig << sb) / rhs_sig`, so it carries one
  // extra binary scaling step compared with the canonical `[sb-1]`-anchored
  // significand representation used by APFloat. Start one exponent below the
  // true exponent difference, then let the normalisation shift account for the
  // quotient's actual MSB position.
  int64_t exp = exponent_ - rhs.exponent_ - 1;

  // Normalise quot to sb bits
  unsigned msb = 0;

  for (int i = static_cast<int>(wb) - 1; i >= 0; --i)
    if (quot.GetBit(i)) {
      msb = i;
      break;
    }

  bool guard = false, round_bit = false;
  unsigned target_msb = sb - 1;

  if (msb > target_msb) {
    unsigned shift = msb - target_msb;
    exp += static_cast<int64_t>(shift);
    if (shift >= 1) guard = quot.GetBit(shift - 1);
    if (shift >= 2) round_bit = quot.GetBit(shift - 2);

    if (shift >= 3) {
      APInt sm = APInt::GetAllOnes(shift - 2).ZExt(wb);
      sticky |= !(quot & sm).IsZero();
    }

    quot = quot.LShr(shift);
  } else if ((int)msb < (int)target_msb) {
    unsigned shift = target_msb - msb;
    exp -= static_cast<int64_t>(shift);
    quot = quot.Shl(shift);
  }

  APInt sig = quot.Trunc(sb);
  APFloat result = FromParts(f, result_sign, exp, sig);
  bool lsb = sig.GetBit(0);
  result.RoundToNearest(lsb, guard, round_bit, sticky, rm, st);
  result.Normalise();

  if (result.exponent_ > f.EMax()) {
    if (st) {
      st->overflow = true;
      st->inexact = true;
    }

    bool to_inf = true;
    if (rm == RoundingMode::kTowardZero) to_inf = false;
    if (rm == RoundingMode::kTowardPositive && result_sign) to_inf = false;
    if (rm == RoundingMode::kTowardNegative && !result_sign) to_inf = false;

    return to_inf ? GetInf(f, result_sign) : GetLargest(f, result_sign);
  }

  if (result.exponent_ < f.EMin() && !result.IsZero()) {
    if (st) st->underflow = true;

    result.category_ = FloatCategory::kSubnormal;
  }

  return result;
}

inline APFloat APFloat::Sqrt(FloatStatus* st, RoundingMode rm) const {
  FloatFormat f = fmt_;

  if (IsNaN()) {
    if (st) st->invalid_op = true;
    return MakeQNaN(f);
  }

  if (IsZero()) return *this;
  if (sign_) {
    if (st) st->invalid_op = true;
    return MakeQNaN(f);
  }

  if (IsInfinity()) return *this;

  // sqrt(m * 2^e) = sqrt(m) * 2^(e/2)
  // Ensure exponent is even by adjusting significand
  unsigned sb = f.SigBits();
  int64_t exp = exponent_;
  APInt sig = significand_.ZExt(sb + 1);

  // Ensure exponent is even so sqrt(2^exp) = 2^(exp/2) exactly
  if (exp & 1) {
    sig = sig.Shl(1);
    --exp;
  }
  exp >>= 1;

  // `sig` is already scaled so that a normal significand represents
  // `sig / 2^(sb-1)`. To keep the square root in that same fixed-point space,
  // compute sqrt(sig * 2^(sb-1)), not sqrt(sig * 2^sb).
  unsigned wb = sb * 2 + 4;
  APInt S = sig.ZExt(wb).Shl(sb - 1);

  // Initial estimate via bit-length
  unsigned Smsb = 0;
  for (int i = static_cast<int>(wb) - 1; i >= 0; --i)
    if (S.GetBit(i)) {
      Smsb = i;
      break;
    }

  APInt x = APInt(wb, 0u);
  x.SetBit(Smsb / 2 + 1);

  // Newton: x = (x + S/x) / 2
  for (int iter = 0; iter < 128; ++iter) {
    if (x.IsZero()) {
      x = APInt(wb, 1u);
    }
    APInt x1 = (x + S.UDiv(x)).LShr(1);
    if (x1.UGe(x)) break;  // converged
    x = x1;
  }
  // x = floor(sqrt(S)); verify x^2 <= S < (x+1)^2
  while (!((x + APInt(wb, 1u)) * (x + APInt(wb, 1u))).UGt(S))
    x += APInt(wb, 1u);
  while ((x * x).UGt(S)) x -= APInt(wb, 1u);

  APInt rem = S - x * x;
  bool sticky = !rem.IsZero();

  // x has sb+something bits; find MSB and normalise to sb bits
  unsigned msb = 0;
  for (int i = static_cast<int>(wb) - 1; i >= 0; --i)
    if (x.GetBit(i)) {
      msb = i;
      break;
    }

  unsigned target_msb = sb - 1;
  bool guard = false, round_bit = false;

  if (msb >= target_msb) {
    unsigned shift = msb - target_msb;
    exp += static_cast<int64_t>(shift);
    if (shift >= 1) guard = x.GetBit(shift - 1);
    if (shift >= 2) round_bit = x.GetBit(shift - 2);
    if (shift >= 3) {
      APInt sm = APInt::GetAllOnes(shift - 2).ZExt(wb);
      sticky |= !(x & sm).IsZero();
    }
    x = x.LShr(shift);
  } else if (msb < target_msb) {
    unsigned shift = target_msb - msb;
    x = x.Shl(shift);
    exp -= static_cast<int64_t>(shift);
  }

  APInt resultSig = x.Trunc(sb);
  APFloat result = FromParts(f, false, exp, resultSig);
  bool lsb = resultSig.GetBit(0);
  result.RoundToNearest(lsb, guard, round_bit, sticky, rm, st);
  result.Normalise();

  return result;
}

inline APFloat APFloat::FusedMulAdd(const APFloat& m, const APFloat& a,
                                    FloatStatus* st, RoundingMode rm) const {
  assert(fmt_ == m.fmt_ && fmt_ == a.fmt_);

  FloatStatus mul_st;
  APFloat product = Mul(m, &mul_st, rm);

  FloatStatus add_st;
  APFloat result = product.Add(a, &add_st, rm);

  if (st) *st |= mul_st;
  if (st) *st |= add_st;

  return result;
}

inline APFloat APFloat::Mod(const APFloat& rhs, FloatStatus* st) const {
  assert(fmt_ == rhs.fmt_);

  if (IsNaN() || rhs.IsNaN() || IsInfinity() || rhs.IsZero()) {
    if (st) st->invalid_op = true;

    return MakeQNaN(fmt_);
  }

  if (IsZero() || rhs.IsInfinity()) return *this;

  APFloat q = Div(rhs, nullptr, RoundingMode::kNearestEven);
  APInt n = q.ConvertToInteger(64, true, nullptr, RoundingMode::kNearestEven);
  APFloat nf =
      ConvertFromAPInt(fmt_, n, true, nullptr, RoundingMode::kNearestEven);

  return Sub(nf.Mul(rhs, nullptr, RoundingMode::kNearestEven), st,
             RoundingMode::kNearestEven);
}

inline APFloat::CmpResult APFloat::Compare(const APFloat& rhs) const {
  assert(fmt_ == rhs.fmt_);
  if (IsNaN() || rhs.IsNaN()) return CmpResult::kUnordered;

  // Zeros: +0 == -0
  if (IsZero() && rhs.IsZero()) return CmpResult::kEqual;
  if (IsZero()) return rhs.sign_ ? CmpResult::kGreater : CmpResult::kLess;
  if (rhs.IsZero()) return sign_ ? CmpResult::kLess : CmpResult::kGreater;

  if (sign_ != rhs.sign_) return sign_ ? CmpResult::kLess : CmpResult::kGreater;

  bool neg = sign_;

  if (IsInfinity() && rhs.IsInfinity()) return CmpResult::kEqual;
  if (IsInfinity()) return neg ? CmpResult::kLess : CmpResult::kGreater;
  if (rhs.IsInfinity()) return neg ? CmpResult::kGreater : CmpResult::kLess;

  CmpResult r;

  if (exponent_ != rhs.exponent_) {
    r = exponent_ < rhs.exponent_ ? CmpResult::kLess : CmpResult::kGreater;
  } else {
    if (significand_ == rhs.significand_) return CmpResult::kEqual;

    r = significand_.ULt(rhs.significand_) ? CmpResult::kLess
                                           : CmpResult::kGreater;
  }

  if (neg) r = (r == CmpResult::kLess) ? CmpResult::kGreater : CmpResult::kLess;

  return r;
}

inline APFloat APFloat::Convert(FloatFormat dst, FloatStatus* st,
                                RoundingMode rm) const {
  if (fmt_ == dst) return *this;

  if (IsNaN()) {
    APInt payload_bits =
        significand_ & APInt::GetAllOnes(fmt_.mant_bits).ZExt(SigBits());
    uint64_t payload = payload_bits.GetZExtValue();
    if (dst.mant_bits < 64) payload &= ((uint64_t(1) << dst.mant_bits) - 1);

    return GetNaN(dst, sign_, is_signalling_, payload);
  }
  if (IsInfinity()) return GetInf(dst, sign_);
  if (IsZero()) return GetZero(dst, sign_);

  unsigned src_sb = fmt_.SigBits();
  unsigned dst_sb = dst.SigBits();

  APFloat result(dst);
  result.sign_ = sign_;
  result.exponent_ = exponent_;
  result.category_ = category_;
  bool guard = false, round_bit = false, sticky = false;

  if (dst_sb >= src_sb) {
    result.significand_ = significand_.ZExt(dst_sb).Shl(dst_sb - src_sb);
  } else {
    unsigned shift = src_sb - dst_sb;
    APInt sig = significand_.ZExt(src_sb);

    if (shift >= 1) guard = sig.GetBit(shift - 1);
    if (shift >= 2) round_bit = sig.GetBit(shift - 2);

    if (shift >= 3) {
      APInt sm = APInt::GetAllOnes(shift - 2).ZExt(src_sb);
      sticky = !(sig & sm).IsZero();
    }

    result.significand_ = sig.LShr(shift).Trunc(dst_sb);
  }

  bool lsb = result.significand_.GetBit(0);
  bool inexact = guard || round_bit || sticky;
  result.RoundToNearest(lsb, guard, round_bit, sticky, rm, st);
  bool underflowed_to_zero = inexact && result.significand_.IsZero();
  result.Normalise();

  if (result.exponent_ > dst.EMax()) {
    if (st) {
      st->overflow = true;
      st->inexact = true;
    }

    bool to_inf = true;
    if (rm == RoundingMode::kTowardZero) to_inf = false;
    if (rm == RoundingMode::kTowardPositive && sign_) to_inf = false;
    if (rm == RoundingMode::kTowardNegative && !sign_) to_inf = false;

    return to_inf ? GetInf(dst, sign_) : GetLargest(dst, sign_);
  }

  if (underflowed_to_zero ||
      (result.exponent_ < dst.EMin() && !result.IsZero())) {
    if (st) st->underflow = true;

    if (!result.IsZero()) result.category_ = FloatCategory::kSubnormal;
  }

  return result;
}

inline APInt APFloat::ConvertToInteger(unsigned int_bits, bool is_signed,
                                       FloatStatus* st, RoundingMode rm) const {
  if (IsNaN()) {
    if (st) st->invalid_op = true;

    return APInt(int_bits, 0u);
  }

  APInt max_val = is_signed ? APInt::GetSignedMaxValue(int_bits)
                            : APInt::GetMaxValue(int_bits);
  APInt min_val =
      is_signed ? APInt(int_bits,
                        static_cast<uint64_t>(-(int64_t(1) << (int_bits - 1))))
                : APInt::GetZero(int_bits);

  if (IsInfinity() || IsZero()) {
    if (IsInfinity()) {
      if (st) st->invalid_op = true;

      return sign_ ? min_val : max_val;
    }

    return APInt(int_bits, 0u);
  }

  int64_t shift = exponent_ - static_cast<int64_t>(SigBits() - 1);
  APInt sig = significand_;
  bool inexact = false;
  bool guard = false;
  bool sticky = false;

  if (shift < 0) {
    unsigned discard =
        static_cast<unsigned>(std::min(-shift, (int64_t)SigBits()));
    if (discard >= 1) guard = sig.GetBit(discard - 1);
    if (discard >= 2) {
      APInt stickMask = APInt::GetAllOnes(discard - 1).ZExt(SigBits());
      sticky = !(sig & stickMask).IsZero();
    }
    inexact = guard || sticky;
    sig = sig.LShr(discard);
  } else if (shift > 0) {
    sig = sig.ZExt(SigBits() + static_cast<unsigned>(shift))
              .Shl(static_cast<unsigned>(shift));
  }

  if (inexact) {
    if (st) st->inexact = true;

    bool inc = false;

    if (rm == RoundingMode::kTowardPositive && !sign_) inc = true;
    if (rm == RoundingMode::kTowardNegative && sign_) inc = true;
    if (rm == RoundingMode::kNearestAway) inc = guard;
    if (rm == RoundingMode::kNearestEven)
      inc = guard && (sticky || sig.GetBit(0));

    if (inc) sig += APInt(sig.GetBitWidth(), 1u);
  }

  unsigned cmp_width = std::max(sig.GetBitWidth(), int_bits + 1);
  APInt sig_cmp = sig.GetBitWidth() == cmp_width ? sig : sig.ZExt(cmp_width);

  if (is_signed) {
    APInt pos_limit = APInt::GetSignedMaxValue(int_bits).ZExt(cmp_width);
    APInt neg_limit(cmp_width, 1u);
    neg_limit = neg_limit.Shl(int_bits - 1);

    if (!sign_ && sig_cmp.UGt(pos_limit)) {
      if (st) st->invalid_op = true;

      return max_val;
    }

    if (sign_ && sig_cmp.UGt(neg_limit)) {
      if (st) st->invalid_op = true;

      return min_val;
    }
  } else {
    if (sign_) {
      if (st) st->invalid_op = true;

      return APInt(int_bits, 0u);
    }

    APInt unsigned_limit = APInt::GetMaxValue(int_bits).ZExt(cmp_width);
    if (sig_cmp.UGt(unsigned_limit)) {
      if (st) st->invalid_op = true;

      return max_val;
    }
  }

  APInt result =
      sig.GetBitWidth() >= int_bits ? sig.Trunc(int_bits) : sig.ZExt(int_bits);

  if (is_signed && sign_) result = -result;

  return result;
}

inline APFloat APFloat::ConvertFromAPInt(FloatFormat fmt, const APInt& val,
                                         bool is_signed, FloatStatus* st,
                                         RoundingMode rm) {
  if (val.IsZero()) return GetZero(fmt);

  bool negative = is_signed && val.IsNegative();
  APInt mag = negative ? -val : val;

  // Find MSB
  unsigned msb = 0;
  for (int i = static_cast<int>(mag.GetBitWidth()) - 1; i >= 0; --i)
    if (mag.GetBit(i)) {
      msb = i;
      break;
    }

  unsigned sb = fmt.SigBits();
  int64_t exp = static_cast<int64_t>(msb);
  bool guard = false, round_bit = false, sticky = false;
  APInt sig;

  if (msb + 1 >= sb) {
    unsigned shift = msb + 1 - sb;

    if (shift >= 1) guard = mag.GetBit(shift - 1);
    if (shift >= 2) round_bit = mag.GetBit(shift - 2);
    if (shift >= 3) {
      APInt sm = APInt::GetAllOnes(shift - 2).ZExt(mag.GetBitWidth());
      sticky = !(mag & sm).IsZero();
    }
    mag = mag.LShr(shift);
    sig = mag.Trunc(sb);
  } else {
    // `mag` may have a wider storage width than the destination significand
    // even when its numeric value is tiny (e.g. parsing a quad hex literal via
    // a 256-bit accumulator). Truncate-or-extend first so we never "extend"
    // into a narrower APInt and scribble past the destination buffer.
    sig = mag.ZExtOrTrunc(sb).Shl(sb - msb - 1);
    exp = static_cast<int64_t>(msb);
  }

  APFloat result = FromParts(fmt, negative, exp, sig);
  bool lsb = sig.GetBit(0);
  result.RoundToNearest(lsb, guard, round_bit, sticky, rm, st);
  result.Normalise();

  if (result.exponent_ > fmt.EMax()) {
    if (st) {
      st->overflow = true;
      st->inexact = true;
    }

    bool to_inf = true;
    if (rm == RoundingMode::kTowardZero) to_inf = false;
    if (rm == RoundingMode::kTowardPositive && negative) to_inf = false;
    if (rm == RoundingMode::kTowardNegative && !negative) to_inf = false;

    return to_inf ? GetInf(fmt, negative) : GetLargest(fmt, negative);
  }

  return result;
}

inline std::string APFloat::ToHexString() const {
  if (IsNaN()) return sign_ ? "-nan" : "nan";
  if (IsInfinity()) return sign_ ? "-inf" : "inf";
  if (IsZero()) return sign_ ? "-0x0p+0" : "0x0p+0";

  // For binary16/32/64, reuse C hex-float formatting from an exact double.
  if (fmt_.SigBits() <= 53) {
    double v = ToDouble();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%a", v);

    return buf;
  }

  // Exact wide-format encoding in normalized hexadecimal scientific notation.
  std::string s = sign_ ? "-0x1" : "0x1";
  unsigned sb = SigBits();

  int msb = static_cast<int>(sb) - 1;
  while (msb > 0 && !significand_.GetBit(static_cast<unsigned>(msb))) --msb;

  auto hex_char = [](uint8_t v) {
    return static_cast<char>(v < 10 ? '0' + v : 'a' + (v - 10));
  };

  if (msb > 0) {
    std::string frac;
    for (int group_hi = msb - 1; group_hi >= 0; group_hi -= 4) {
      uint8_t nib = 0;
      for (int b = 0; b < 4; ++b) {
        int bit = group_hi - b;
        if (bit >= 0 && significand_.GetBit(static_cast<unsigned>(bit))) {
          nib |= static_cast<uint8_t>(1u << (3 - b));
        }
      }
      frac += hex_char(nib);
    }

    while (!frac.empty() && frac.back() == '0') frac.pop_back();
    if (!frac.empty()) {
      s += '.';
      s += frac;
    }
  }

  int64_t exp = exponent_ - static_cast<int64_t>(sb - 1) + msb;
  s += 'p';
  if (exp >= 0) s += '+';
  s += std::to_string(exp);

  return s;
}

inline std::string APFloat::ToString(unsigned, unsigned) const {
  if (IsNaN()) return sign_ ? "-nan" : "nan";
  if (IsInfinity()) return sign_ ? "-inf" : "inf";
  if (IsZero()) return sign_ ? "-0" : "0";

  // Convert via double for now (accurate for <= 53-bit significand formats).
  // For Quad precision, emit hex string.

  // TODO(gc): double check this.
  if (fmt_.SigBits() > 80) return ToHexString();
  // if (fmt_.SigBits() > 53) return ToHexString();

  double v = ToDouble();
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.17g", v);

  return buf;
}

inline APFloat::APFloat(FloatFormat fmt, std::string_view str, FloatStatus* st,
                        RoundingMode rm)
    : APFloat(fmt) {
  if (str.empty()) return;

  size_t pos = 0;
  bool neg = false;

  if (str[pos] == '-') {
    pos++;
    neg = true;
  } else if (str[pos] == '+') {
    pos++;
  }

  std::string_view rest = str.substr(pos);
  std::string restl(rest);
  for (char& c : restl) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }

  if (restl == "inf" || restl == "infinity") {
    *this = GetInf(fmt, neg);

    return;
  }

  if (restl.starts_with("nan")) {
    *this = GetNaN(fmt, neg, false, 0);

    return;
  }

  if (restl.starts_with("0x")) {
    char* endptr = nullptr;
    std::string strbuf(str);

    if (fmt == fmt::kFloat) {
      float v = std::strtof(strbuf.c_str(), &endptr);
      uint32_t bits;
      std::memcpy(&bits, &v, sizeof(bits));
      *this = APFloat(fmt, APInt(32, bits));
      return;
    }

    if (fmt == fmt::kDouble) {
      double v = std::strtod(strbuf.c_str(), &endptr);
      uint64_t bits;
      std::memcpy(&bits, &v, sizeof(bits));
      *this = APFloat(fmt, APInt(64, bits));
      return;
    }

    // Parse for wider formats: accumulate hexadecimal significand, then scale
    // by powers of two in APFloat arithmetic to avoid lossy double fallback.
    rest = rest.substr(2);
    APInt acc(256, 0u);
    size_t i = 0;
    for (;
         i < rest.size() && rest[i] != '.' && rest[i] != 'p' && rest[i] != 'P';
         ++i) {
      uint8_t d = (rest[i] >= '0' && rest[i] <= '9')   ? rest[i] - '0'
                  : (rest[i] >= 'a' && rest[i] <= 'f') ? rest[i] - 'a' + 10
                  : (rest[i] >= 'A' && rest[i] <= 'F') ? rest[i] - 'A' + 10
                                                       : 255;
      if (d == 255) break;
      acc = acc.Shl(4) + APInt(256, d);
    }

    int64_t frac_bits = 0;
    if (i < rest.size() && rest[i] == '.') {
      ++i;
      for (; i < rest.size() && rest[i] != 'p' && rest[i] != 'P'; ++i) {
        uint8_t d = (rest[i] >= '0' && rest[i] <= '9')   ? rest[i] - '0'
                    : (rest[i] >= 'a' && rest[i] <= 'f') ? rest[i] - 'a' + 10
                    : (rest[i] >= 'A' && rest[i] <= 'F') ? rest[i] - 'A' + 10
                                                         : 255;
        if (d == 255) break;
        acc = acc.Shl(4) + APInt(256, d);
        frac_bits += 4;
      }
    }

    int64_t exp2 = 0;
    if (i < rest.size() && (rest[i] == 'p' || rest[i] == 'P')) {
      ++i;
      bool eneg = false;
      if (i < rest.size() && (rest[i] == '-' || rest[i] == '+')) {
        eneg = (rest[i++] == '-');
      }
      for (; i < rest.size(); ++i) {
        if (rest[i] < '0' || rest[i] > '9') break;
        exp2 = exp2 * 10 + (rest[i] - '0');
      }
      if (eneg) exp2 = -exp2;
    }

    exp2 -= frac_bits;

    if (acc.IsZero()) {
      *this = GetZero(fmt, neg);
      return;
    }

    APFloat a = ConvertFromAPInt(fmt, acc, false, st, rm);

    if (exp2 != 0) {
      a.exponent_ += exp2;

      if (a.exponent_ > fmt.EMax()) {
        if (st) st->overflow = true;
        a = GetInf(fmt, false);
      } else if (a.exponent_ < fmt.EMin()) {
        unsigned shift = static_cast<unsigned>(fmt.EMin() - a.exponent_);
        bool sticky = ShiftRightSticky(a.significand_, shift);
        a.exponent_ = fmt.EMin();

        if (sticky && rm != RoundingMode::kTowardZero) {
          a.significand_ += APInt(a.SigBits(), 1u);
        }

        if (a.significand_.IsZero()) {
          a = GetZero(fmt, false);
        } else if (a.significand_.GetBit(a.SigBits() - 1)) {
          a.category_ = FloatCategory::kNormal;
        } else {
          a.category_ = FloatCategory::kSubnormal;
        }
      } else {
        a.category_ = FloatCategory::kNormal;
      }
    }

    a.sign_ = neg;
    *this = a;

    return;
  }

  // Decimal: parse as double then convert (sufficient for most compiler uses)
  // For truly arbitrary precision, a full Clinger/Ryu parser would be needed.
  char* endptr = nullptr;
  std::string strbuf(str);

  if (fmt == fmt::kFloat) {
    float v = std::strtof(strbuf.c_str(), &endptr);
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    *this = APFloat(fmt, APInt(32, bits));
    return;
  }

  if (fmt == fmt::kDouble) {
    double v = std::strtod(strbuf.c_str(), &endptr);
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    *this = APFloat(fmt, APInt(64, bits));
    return;
  }

  double v = std::strtod(strbuf.c_str(), &endptr);
  *this = APFloat(fmt, v);

  if (st && v != 0.0) {
    // Check if we lost precision (rough check)
  }
}

}  // namespace bcc
