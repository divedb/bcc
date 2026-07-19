#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <compare>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <format>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bcc {

class APInt;
class APSInt;

/// \brief APInt represents an arbitrary-precision integer with a fixed
///        bit-width.
class APInt {
 public:
  static constexpr unsigned kBitsPerWord = 64u;

  using WordType = uint64_t;

  /// \brief Get zero value of APInt with the specified bit width.
  ///
  /// \param bits The bit width of the APInt.
  /// \return     An APInt representing zero with the specified bit width.
  static APInt GetZero(unsigned bits) { return APInt(bits, 0u); }

  /// \brief Get an APInt with all bits set to 1 for the specified bit width.
  ///
  /// \param bits The bit width of the APInt.
  /// \return     An APInt with all bits set to 1.
  static APInt GetAllOnes(unsigned bits) {
    APInt r(bits, 0);
    r.SetAllBits();

    return r;
  }

  /// \brief Get the minimum unsigned value for the specified bit width.
  ///
  /// \param bits The bit width of the APInt.
  /// \return     An APInt representing the minimum unsigned value.
  static APInt GetMinValue(unsigned bits) { return GetZero(bits); }

  /// \brief Get the maximum unsigned value for the specified bit width.
  ///
  /// \param bits The bit width of the APInt.
  /// \return     An APInt representing the maximum unsigned value.
  static APInt GetMaxValue(unsigned bits) { return GetAllOnes(bits); }

  static APInt GetSignedMinValue(unsigned bits) {
    APInt t(bits, 0u);
    t.SetBit(bits - 1);

    return t;
  }

  static APInt GetSignedMaxValue(unsigned bits) {
    return GetAllOnes(bits).ClearBit(bits - 1);
  }

  static APInt GetOneBitSet(unsigned bits, unsigned pos) {
    return APInt(bits, 0u).SetBit(pos);
  }

  /// \brief Default constructor for an APInt with a bit width of 1 and a value
  ///        of 0.
  APInt() : APInt(1, 0u) {}

  /// \brief Construct an APInt with the given bit width and value.
  ///
  /// \param bit_width The bit width of the APInt. Must be at least 1.
  /// \param val       The initial value of the APInt. Only the least
  ///                  significant `bit_width` bits are used.
  ///
  /// \param is_signed Whether the value should be treated as signed.
  APInt(unsigned bit_width, uint64_t val, bool /*is_signed*/ = false)
      : bit_width_(bit_width) {
    assert(bit_width_ >= 1 && "APInt requires at least 1 bit");

    if (IsSingleWord()) {
      val_ = val;
    } else {
      AllocWords(NumWords());
      words_[0] = val;
    }

    ClearUnusedBits();
  }

  /// \brief Construct an APInt with the given bit width and initial value
  ///        specified as a list of words.
  ///
  /// \param bit_width The bit width of the APInt. Must be at least 1.
  /// \param words     The initial value of the APInt as a list of words.
  APInt(unsigned bit_width, std::span<const uint64_t> words)
      : bit_width_(bit_width) {
    assert(bit_width >= 1);

    unsigned nw = NumWords();

    if (IsSingleWord()) {
      val_ = words.empty() ? 0 : words[0];
    } else {
      AllocWords(nw);
      unsigned to_copy = std::min(static_cast<unsigned>(words.size()), nw);

      for (unsigned i = 0; i < to_copy; ++i) words_[i] = words[i];
    }

    ClearUnusedBits();
  }

  /// \brief Construct an APInt from a string representation.
  ///
  /// \param bit_width The bit width of the APInt. Must be at least 1.
  /// \param str       The string representation of the value.
  APInt(unsigned bit_width, std::string_view str);

  /// \brief Copy constructor for APInt.
  ///
  /// \param o The APInt to copy.
  APInt(const APInt &o) : bit_width_(o.bit_width_) {
    if (IsSingleWord()) {
      val_ = o.val_;
    } else {
      AllocWords(NumWords());
      std::copy_n(o.words_, NumWords(), words_);
    }
  }

  /// \brief Move constructor for APInt.
  ///
  /// \param o The APInt to move.
  APInt(APInt &&o) noexcept : bit_width_(o.bit_width_) {
    if (IsSingleWord()) {
      val_ = o.val_;
    } else {
      words_ = o.words_;
      o.words_ = nullptr;
      o.bit_width_ = 0;
    }
  }

  /// \brief Copy assignment operator for APInt.
  ///
  /// \param o The APInt to copy.
  /// \return  A reference to this APInt.
  APInt &operator=(const APInt &o) {
    if (this == &o) return *this;

    if (!IsSingleWord() && words_) delete[] words_;

    bit_width_ = o.bit_width_;

    if (IsSingleWord()) {
      val_ = o.val_;
    } else {
      AllocWords(NumWords());
      std::copy_n(o.words_, NumWords(), words_);
    }

    return *this;
  }

  /// \brief Move assignment operator for APInt.
  ///
  /// \param o The APInt to move.
  /// \return  A reference to this APInt.
  APInt &operator=(APInt &&o) noexcept {
    if (this == &o) return *this;
    if (!IsSingleWord() && words_) delete[] words_;

    bit_width_ = o.bit_width_;

    if (IsSingleWord()) {
      val_ = o.val_;
    } else {
      words_ = o.words_;
      o.words_ = nullptr;
      o.bit_width_ = 0;
    }

    return *this;
  }

  ~APInt() {
    if (!IsSingleWord() && words_) delete[] words_;
  }

  unsigned GetBitWidth() const { return bit_width_; }
  unsigned NumWords() const { return WordCount(bit_width_); }

  /// \brief Returns true if the APInt fits in a single word (64 bits).
  ///
  /// \return true if the APInt fits in a single word; otherwise, false.
  bool IsSingleWord() const noexcept { return bit_width_ <= kBitsPerWord; }

  /// \brief Get the value of the bit at position `pos`.
  ///
  /// \param pos The position of the bit to get.
  /// \return    The value of the bit at position `pos`.
  bool GetBit(unsigned pos) const {
    if (pos >= bit_width_) return false;
    return (WordAt(pos / kBitsPerWord) >> (pos % kBitsPerWord)) & 1u;
  }

  /// \brief Set the bit at position `pos`.
  ///
  /// \param pos The position of the bit to set.
  /// \return    A reference to this APInt.
  APInt &SetBit(unsigned pos) {
    assert(pos < bit_width_);

    WordRef(pos / kBitsPerWord) |= WordType(1) << (pos % kBitsPerWord);

    return *this;
  }

  /// \brief Clear the bit at position `pos`.
  ///
  /// \param pos The position of the bit to clear.
  /// \return    A reference to this APInt.
  APInt &ClearBit(unsigned pos) {
    assert(pos < bit_width_);

    WordRef(pos / kBitsPerWord) &= ~(WordType(1) << (pos % kBitsPerWord));

    return *this;
  }

  /// \brief Flip the bit at position `pos`.
  ///
  /// \param pos The position of the bit to flip.
  /// \return    A reference to this APInt.
  APInt &FlipBit(unsigned pos) {
    assert(pos < bit_width_);

    WordRef(pos / kBitsPerWord) ^= WordType(1) << (pos % kBitsPerWord);

    return *this;
  }

  /// \brief Set all bits to 1.
  ///
  /// \return A reference to this APInt.
  APInt &SetAllBits() {
    for (unsigned i = 0; i < NumWords(); ++i) WordRef(i) = ~WordType(0);

    ClearUnusedBits();

    return *this;
  }

  APInt &ClearAllBits() {
    for (unsigned i = 0; i < NumWords(); ++i) WordRef(i) = 0;

    return *this;
  }

  /// \brief Add two APInt values.
  ///
  /// \param rhs The APInt to add.
  /// \return    A new APInt representing the sum.
  APInt operator+(const APInt &rhs) const {
    auto r = *this;
    r += rhs;

    return r;
  }

  /// \brief Subtract two APInt values.
  ///
  /// \param rhs The APInt to subtract.
  /// \return    A new APInt representing the difference.
  APInt operator-(const APInt &rhs) const {
    auto r = *this;
    r -= rhs;

    return r;
  }

  /// \brief Multiply two APInt values.
  ///
  /// \param rhs The APInt to multiply.
  /// \return    A new APInt representing the product.
  APInt operator*(const APInt &rhs) const {
    auto r = *this;
    r *= rhs;

    return r;
  }

  /// \brief Divide two APInt values (unsigned).
  ///
  /// \param rhs The APInt to divide by.
  /// \return    A new APInt representing the quotient.
  APInt operator/(const APInt &rhs) const { return UDiv(rhs); }

  /// \brief Compute the remainder of two APInt values (unsigned).
  ///
  /// \param rhs The APInt to divide by.
  /// \return    A new APInt representing the remainder.
  APInt operator%(const APInt &rhs) const { return URem(rhs); }

  /// Unary negation: two's complement negate = ~x + 1
  APInt operator-() const {
    APInt r(bit_width_, 0u);
    // ~*this + 1
    for (unsigned i = 0; i < NumWords(); ++i) r.WordRef(i) = ~WordAt(i);

    r.ClearUnusedBits();
    r.AddWord(0, 1);
    r.ClearUnusedBits();

    return r;
  }
  APInt operator~() const {
    auto r = *this;
    for (unsigned i = 0; i < NumWords(); ++i) r.WordRef(i) = ~r.WordAt(i);
    r.ClearUnusedBits();
    return r;
  }

  APInt &operator+=(const APInt &rhs) {
    assert(SameBits(rhs));
    AddInPlace(rhs);
    ClearUnusedBits();
    return *this;
  }
  APInt &operator-=(const APInt &rhs) {
    assert(SameBits(rhs));
    *this += (-rhs);
    return *this;
  }
  APInt &operator*=(const APInt &rhs) {
    assert(SameBits(rhs));
    *this = Multiply(*this, rhs);
    return *this;
  }

  APInt &operator++() {
    AddWord(0, 1);
    ClearUnusedBits();

    return *this;
  }

  APInt &operator--() {
    *this -= APInt(bit_width_, 1u);

    return *this;
  }

  APInt operator++(int) {
    auto t = *this;
    ++(*this);

    return t;
  }

  APInt operator--(int) {
    auto t = *this;
    --(*this);

    return t;
  }

  APInt operator&(const APInt &rhs) const {
    auto r = *this;
    r &= rhs;

    return r;
  }

  APInt operator|(const APInt &rhs) const {
    auto r = *this;
    r |= rhs;

    return r;
  }

  APInt operator^(const APInt &rhs) const {
    auto r = *this;
    r ^= rhs;

    return r;
  }

  APInt &operator&=(const APInt &rhs) {
    assert(SameBits(rhs));

    for (unsigned i = 0; i < NumWords(); ++i) WordRef(i) &= rhs.WordAt(i);

    return *this;
  }

  APInt &operator|=(const APInt &rhs) {
    assert(SameBits(rhs));
    for (unsigned i = 0; i < NumWords(); ++i) WordRef(i) |= rhs.WordAt(i);

    return *this;
  }

  APInt &operator^=(const APInt &rhs) {
    assert(SameBits(rhs));
    for (unsigned i = 0; i < NumWords(); ++i) WordRef(i) ^= rhs.WordAt(i);

    return *this;
  }

  APInt operator<<(unsigned shift) const { return Shl(shift); }
  APInt operator>>(unsigned shift) const { return LShr(shift); }
  APInt &operator<<=(unsigned shift) {
    *this = Shl(shift);

    return *this;
  }

  APInt &operator>>=(unsigned shift) {
    *this = LShr(shift);

    return *this;
  }

  void LShrInPlace(unsigned shift) { *this = LShr(shift); }
  void AShrInPlace(unsigned shift) { *this = AShr(shift); }

  APInt RelativeLShl(unsigned shift) const { return Shl(shift); }
  APInt RelativeLShr(unsigned shift) const { return LShr(shift); }
  APInt RelativeAShl(unsigned shift) const { return Shl(shift); }
  APInt RelativeAShr(unsigned shift) const { return AShr(shift); }

  APInt Shl(unsigned shift) const;
  APInt LShr(unsigned shift) const;
  APInt AShr(unsigned shift) const;

  bool operator==(const APInt &rhs) const {
    if (bit_width_ != rhs.bit_width_) return false;

    for (unsigned i = 0; i < NumWords(); ++i) {
      if (WordAt(i) != rhs.WordAt(i)) return false;
    }

    return true;
  }

  bool operator!=(const APInt &rhs) const { return !(*this == rhs); }

  bool ULt(const APInt &rhs) const;
  bool ULe(const APInt &rhs) const { return !rhs.ULt(*this); }
  bool UGt(const APInt &rhs) const { return rhs.ULt(*this); }
  bool UGe(const APInt &rhs) const { return !ULt(rhs); }

  bool SLt(const APInt &rhs) const;
  bool SLe(const APInt &rhs) const { return !rhs.SLt(*this); }
  bool SGt(const APInt &rhs) const { return rhs.SLt(*this); }
  bool SGe(const APInt &rhs) const { return !SLt(rhs); }

  APInt UDiv(const APInt &rhs) const;
  APInt URem(const APInt &rhs) const;
  APInt SDiv(const APInt &rhs) const;
  APInt SRem(const APInt &rhs) const;

  /// \brief Get the value of this APInt as a zero-extended uint64_t. Only valid
  ///        if the APInt fits in 64 bits.
  ///
  /// \return The value of this APInt as a zero-extended uint64_t.
  uint64_t GetZExtValue() const { return WordAt(0); }

  /// \brief Get the value of this APInt as a sign-extended int64_t. Only valid
  ///        if the APInt fits in 64 bits.
  ///
  /// \return The value of this APInt as a sign-extended int64_t.
  int64_t GetSExtValue() const {
    uint64_t v = GetZExtValue();

    if (bit_width_ < kBitsPerWord && GetBit(bit_width_ - 1)) {
      v |= ~((uint64_t(1) << bit_width_) - 1);
    }

    return static_cast<int64_t>(v);
  }

  /// \brief Check if the APInt value is zero.
  ///
  /// \return true if the APInt value is zero; otherwise, false.
  bool IsZero() const {
    for (unsigned i = 0; i < NumWords(); ++i) {
      if (WordAt(i)) return false;
    }

    return true;
  }

  /// \brief Check if the APInt value is one.
  ///
  /// \return true if the APInt value is one; otherwise, false.
  bool IsOne() const { return *this == APInt(bit_width_, 1u); }

  /// \brief Check if the APInt value has all bits set to 1, is negative (most
  ///        significant bit is set), is non-negative (most significant bit is
  ///        not set), is the minimum signed value, or is the maximum signed
  ///        value.
  ///
  /// \return true if the APInt value satisfies the condition; otherwise, false.
  bool IsAllOnes() const { return *this == GetAllOnes(bit_width_); }
  bool IsNegative() const { return GetBit(bit_width_ - 1); }
  bool IsNonNegative() const { return !IsNegative(); }

  /// \brief Check if the APInt value is the minimum signed value.
  ///
  /// \return true if the APInt value is the minimum signed value; otherwise,
  ///         false.
  bool IsMinSignedValue() const {
    APInt t(bit_width_, 0u);
    t.SetBit(bit_width_ - 1);

    return *this == t;
  }

  /// \brief Check if the APInt value is the maximum signed value.
  ///
  /// \return true if the APInt value is the maximum signed value; otherwise,
  ///         false.
  bool IsMaxSignedValue() const {
    return *this == GetSignedMaxValue(bit_width_);
  }

  /// \brief Check if the APInt value is a power of 2 (i.e., exactly one bit is
  ///        set).
  ///
  /// \return true if the APInt value is a power of 2; otherwise, false.
  bool IsPowerOf2() const;

  unsigned CountLeadingZeros() const;
  unsigned CountLeadingOnes() const;
  unsigned CountTrailingZeros() const;
  unsigned CountTrailingOnes() const;
  unsigned Popcount() const;

  bool IsIntN(unsigned n) const {
    if (n == 0) return IsZero();
    if (bit_width_ <= n) return true;

    return Trunc(n).ZExt(bit_width_) == *this;
  }

  bool IsSignedIntN(unsigned n) const {
    if (n == 0) return false;
    if (bit_width_ <= n) return true;

    return Trunc(n).SExt(bit_width_) == *this;
  }

  int Compare(const APInt &rhs) const {
    assert(SameBits(rhs));

    if (ULt(rhs)) return -1;
    if (rhs.ULt(*this)) return 1;

    return 0;
  }

  int CompareSigned(const APInt &rhs) const {
    assert(SameBits(rhs));

    if (SLt(rhs)) return -1;
    if (rhs.SLt(*this)) return 1;

    return 0;
  }

  APInt ZExt(unsigned new_width) const;
  APInt SExt(unsigned new_width) const;
  APInt Trunc(unsigned new_width) const;
  APInt ZExtOrTrunc(unsigned new_width) const {
    return new_width > bit_width_ ? ZExt(new_width) : Trunc(new_width);
  }

  APInt SExtOrTrunc(unsigned new_width) const {
    return new_width > bit_width_ ? SExt(new_width) : Trunc(new_width);
  }

  std::string ToString(unsigned radix = 10, bool is_signed = false) const;

  std::span<const WordType> GetRawWords() const {
    return IsSingleWord() ? std::span<const WordType>(&val_, 1)
                          : std::span<const WordType>(words_, NumWords());
  }

  /// \brief Returns the word at the given index for read access.
  ///
  /// \param idx The index of the word to access.
  /// \return    The word at the given index.
  WordType WordAt(unsigned idx) const {
    assert(idx < NumWords());

    WordType word = IsSingleWord() ? val_ : words_[idx];

    if (idx + 1 == NumWords()) word &= TopWordMask();

    return word;
  }

  WordType &WordAt(unsigned idx) { return IsSingleWord() ? val_ : words_[idx]; }

 private:
  /// \brief Returns the number of words required to represent the given number
  ///        of bits.
  ///
  /// \param bits The number of bits.
  /// \return     The number of words required.
  static unsigned WordCount(unsigned bits) {
    return (bits + kBitsPerWord - 1) / kBitsPerWord;
  }

  /// \brief Allocate storage for the given number of words and zero-initialize
  ///        them.
  ///
  /// \param n The number of words to allocate.
  void AllocWords(unsigned n) { words_ = new WordType[n](); }

  /// \brief Returns a reference to the word at the given index for mutation.
  ///
  /// \param idx The index of the word to access.
  /// \return    A reference to the word at the given index.
  WordType &WordRef(unsigned idx) {
    return IsSingleWord() ? val_ : words_[idx];
  }

  /// \brief Returns a mask for the top word to clear unused bits in the most
  ///        significant word.
  ///
  /// EXAMPLE:
  /// For a 130-bit APInt (3 words), the top word has 2 bits used, so the mask
  /// would be 0b11 (3 in decimal) to keep those bits and clear the rest.
  ///
  /// \return A mask for the top word to clear unused bits.
  WordType TopWordMask() const {
    unsigned excess = bit_width_ % kBitsPerWord;

    return excess ? (WordType(1) << excess) - 1 : ~WordType(0);
  }

  /// \brief Clears any bits that are unused in the most significant word to
  ///        ensure the value is properly masked to the specified bit width.
  void ClearUnusedBits() {
    if (IsSingleWord()) {
      if (bit_width_ < kBitsPerWord) val_ &= (WordType(1) << bit_width_) - 1;
    } else {
      words_[NumWords() - 1] &= TopWordMask();
    }
  }

  /// \brief Checks if this APInt has the same bit width as another APInt.
  ///
  /// \param o The other APInt to compare with.
  /// \return  true if both APInts have the same bit width; otherwise, false.
  bool SameBits(const APInt &o) const { return bit_width_ == o.bit_width_; }

  /// \brief Adds another APInt to this one in place.
  ///
  /// \param rhs The APInt to add to this one.
  void AddInPlace(const APInt &rhs) {
    if (IsSingleWord()) {
      val_ += rhs.val_;
      return;
    }

    uint64_t carry = 0;
    for (unsigned i = 0; i < NumWords(); ++i) {
      __uint128_t s = __uint128_t(WordAt(i)) + rhs.WordAt(i) + carry;
      WordRef(i) = uint64_t(s);
      carry = uint64_t(s >> 64);
    }
  }

  void AddWord(unsigned start_word, uint64_t carry) {
    if (IsSingleWord()) {
      val_ += carry;
      return;
    }

    for (unsigned i = start_word; i < NumWords() && carry; ++i) {
      __uint128_t s = __uint128_t(words_[i]) + carry;
      words_[i] = uint64_t(s);
      carry = uint64_t(s >> 64);
    }
  }

  static APInt Multiply(const APInt &lhs, const APInt &rhs);

  static void KnuthDivision(const APInt &lhs, const APInt &rhs, APInt *quotient,
                            APInt *remainder);

  unsigned bit_width_ = 1;

  union {
    WordType val_ = 0;
    WordType *words_;
  };
};

/// \brief Shift left (logical/arithmetic) operation.
///
/// \param shift The number of bits to shift.
/// \return      A new APInt with the result of the shift.
inline APInt APInt::Shl(unsigned shift) const {
  if (shift >= bit_width_) return GetZero(bit_width_);
  if (IsSingleWord()) return APInt(bit_width_, val_ << shift);

  APInt result(bit_width_, 0u);
  unsigned word_shift = shift / kBitsPerWord;
  unsigned bit_shift = shift % kBitsPerWord;
  unsigned nw = NumWords();

  for (unsigned i = nw - 1; i >= word_shift; --i) {
    unsigned src = i - word_shift;
    result.words_[i] = words_[src] << bit_shift;

    if (bit_shift && src > 0) {
      result.words_[i] |= words_[src - 1] >> (kBitsPerWord - bit_shift);
    }

    if (i == 0) break;
  }

  result.ClearUnusedBits();

  return result;
}

inline APInt APInt::LShr(unsigned shift) const {
  if (shift >= bit_width_) return GetZero(bit_width_);
  if (IsSingleWord()) return APInt(bit_width_, val_ >> shift);

  APInt result(bit_width_, 0u);
  unsigned word_shift = shift / kBitsPerWord;
  unsigned bit_shift = shift % kBitsPerWord;
  unsigned nw = NumWords();

  for (unsigned i = 0; i + word_shift < nw; ++i) {
    unsigned src = i + word_shift;
    result.words_[i] = words_[src] >> bit_shift;

    if (bit_shift && src + 1 < nw) {
      result.words_[i] |= words_[src + 1] << (kBitsPerWord - bit_shift);
    }
  }

  return result;
}

inline APInt APInt::AShr(unsigned shift) const {
  if (!IsNegative()) return LShr(shift);
  if (shift >= bit_width_) return GetAllOnes(bit_width_);

  if (IsSingleWord()) {
    uint64_t v = GetZExtValue() >> shift;
    // Sign-extend if the sign bit was set
    if (GetBit(bit_width_ - 1) && shift < bit_width_) {
      v |= ~((uint64_t(1) << (bit_width_ - shift)) - 1);
    }

    return APInt(bit_width_, v);
  }

  APInt result = LShr(shift);
  unsigned bits_to_fill = std::min(shift, bit_width_);
  APInt ones = GetAllOnes(bit_width_).Shl(bit_width_ - bits_to_fill);
  result |= ones;
  result.ClearUnusedBits();

  return result;
}

inline bool APInt::ULt(const APInt &rhs) const {
  assert(SameBits(rhs));

  for (int i = static_cast<int>(NumWords()) - 1; i >= 0; --i) {
    if (WordAt(i) < rhs.WordAt(i)) return true;
    if (WordAt(i) > rhs.WordAt(i)) return false;
  }

  return false;
}

inline bool APInt::SLt(const APInt &rhs) const {
  assert(SameBits(rhs));

  bool lhs_neg = IsNegative();
  bool rhs_neg = rhs.IsNegative();

  if (lhs_neg != rhs_neg) return lhs_neg;

  return ULt(rhs);
}

inline bool APInt::IsPowerOf2() const {
  if (IsZero()) return false;

  APInt tmp = *this - APInt(bit_width_, 1u);

  return (*this & tmp).IsZero();
}

inline unsigned APInt::CountTrailingZeros() const {
  for (unsigned i = 0; i < NumWords(); ++i) {
    uint64_t w = WordAt(i);

    if (w) {
      return i * kBitsPerWord + static_cast<unsigned>(std::countr_zero(w));
    }
  }

  return bit_width_;
}

inline unsigned APInt::CountTrailingOnes() const {
  for (unsigned i = 0; i < NumWords(); ++i) {
    uint64_t w = ~WordAt(i);

    if (w) {
      return i * kBitsPerWord + static_cast<unsigned>(std::countr_zero(w));
    }
  }

  return bit_width_;
}

inline unsigned APInt::CountLeadingZeros() const {
  // Top word may have unused high bits that are always zero; subtract them.
  unsigned top_bits = bit_width_ % kBitsPerWord;

  if (top_bits == 0) top_bits = kBitsPerWord;

  unsigned extra = kBitsPerWord - top_bits;

  for (int i = static_cast<int>(NumWords()) - 1; i >= 0; --i) {
    uint64_t w = WordAt(i);

    if (w) {
      unsigned clz = static_cast<unsigned>(std::countl_zero(w));

      return static_cast<unsigned>(NumWords() - 1 - i) * kBitsPerWord + clz -
             extra;
    }
  }

  return bit_width_;
}

inline unsigned APInt::CountLeadingOnes() const {
  unsigned top_bits = bit_width_ % kBitsPerWord;

  if (top_bits == 0) top_bits = kBitsPerWord;

  unsigned extra = kBitsPerWord - top_bits;
  unsigned count = 0;

  for (int i = static_cast<int>(NumWords()) - 1; i >= 0; --i) {
    uint64_t w;

    if (i == static_cast<int>(NumWords()) - 1) {
      w = WordAt(i) << extra;
    } else {
      w = WordAt(i);
    }

    unsigned clo = static_cast<unsigned>(std::countl_one(w));
    count += clo;

    if (clo < kBitsPerWord) break;
  }

  return std::min(count, bit_width_);
}

inline unsigned APInt::Popcount() const {
  unsigned cnt = 0;

  for (unsigned i = 0; i < NumWords(); ++i) {
    cnt += static_cast<unsigned>(std::popcount(WordAt(i)));
  }

  return cnt;
}

inline APInt APInt::ZExt(unsigned new_width) const {
  assert(new_width >= bit_width_);

  if (new_width == bit_width_) return *this;

  APInt result(new_width, 0u);
  unsigned nw = NumWords();

  for (unsigned i = 0; i < nw; ++i) result.WordRef(i) = WordAt(i);

  return result;
}

inline APInt APInt::SExt(unsigned new_width) const {
  assert(new_width >= bit_width_);

  if (new_width == bit_width_) return *this;

  APInt result = ZExt(new_width);

  if (IsNegative()) {
    for (unsigned bit = bit_width_; bit < new_width; ++bit) {
      result.SetBit(bit);
    }
  }

  result.ClearUnusedBits();

  return result;
}

inline APInt APInt::Trunc(unsigned new_width) const {
  assert(new_width <= bit_width_);

  if (new_width == bit_width_) return *this;

  APInt result(new_width, 0u);
  unsigned nw = result.NumWords();

  for (unsigned i = 0; i < nw; ++i) result.WordRef(i) = WordAt(i);

  result.ClearUnusedBits();

  return result;
}

inline APInt APInt::Multiply(const APInt &lhs, const APInt &rhs) {
  unsigned bits = lhs.bit_width_;
  APInt result(bits, 0u);
  unsigned nw = lhs.NumWords();

  for (unsigned i = 0; i < nw; ++i) {
    uint64_t carry = 0;

    for (unsigned j = 0; i + j < nw; ++j) {
      __uint128_t p = __uint128_t(lhs.WordAt(i)) * rhs.WordAt(j) +
                      result.WordAt(i + j) + carry;
      result.WordRef(i + j) = uint64_t(p);
      carry = uint64_t(p >> 64);
    }
  }

  result.ClearUnusedBits();

  return result;
}

inline APInt APInt::UDiv(const APInt &rhs) const {
  assert(!rhs.IsZero() && "Division by zero");
  assert(SameBits(rhs));

  if (ULt(rhs)) return GetZero(bit_width_);
  if (*this == rhs) return APInt(bit_width_, 1u);

  if (IsSingleWord()) return APInt(bit_width_, val_ / rhs.val_);

  APInt quotient(bit_width_, 0u), remainder(bit_width_, 0u);
  KnuthDivision(*this, rhs, &quotient, &remainder);

  return quotient;
}

inline APInt APInt::URem(const APInt &rhs) const {
  assert(!rhs.IsZero() && "Division by zero");
  assert(SameBits(rhs));

  if (ULt(rhs)) return *this;
  if (*this == rhs) return GetZero(bit_width_);

  if (IsSingleWord()) return APInt(bit_width_, val_ % rhs.val_);

  APInt quotient(bit_width_, 0u), remainder(bit_width_, 0u);
  KnuthDivision(*this, rhs, &quotient, &remainder);

  return remainder;
}

inline APInt APInt::SDiv(const APInt &rhs) const {
  bool lhs_neg = IsNegative();
  bool rhs_neg = rhs.IsNegative();
  APInt lhs_abs = lhs_neg ? -(*this) : *this;
  APInt rhs_abs = rhs_neg ? -(rhs) : rhs;
  APInt q = lhs_abs.UDiv(rhs_abs);

  if (lhs_neg != rhs_neg) q = -q;

  return q;
}

inline APInt APInt::SRem(const APInt &rhs) const {
  bool lhs_neg = IsNegative();
  bool rhs_neg = rhs.IsNegative();
  APInt lhs_abs = lhs_neg ? -(*this) : *this;
  APInt rhs_abs = rhs_neg ? -(rhs) : rhs;
  APInt r = lhs_abs.URem(rhs_abs);

  if (lhs_neg) r = -r;

  return r;
}

inline void APInt::KnuthDivision(const APInt &lhs, const APInt &rhs, APInt *Q,
                                 APInt *R) {
  unsigned bits = lhs.bit_width_;
  *Q = APInt(bits, 0u);
  *R = APInt(bits, 0u);

  for (int i = static_cast<int>(bits) - 1; i >= 0; --i) {
    *R <<= 1u;

    if (lhs.GetBit(static_cast<unsigned>(i))) R->SetBit(0);

    if (R->UGe(rhs)) {
      *R -= rhs;
      Q->SetBit(static_cast<unsigned>(i));
    }
  }
}

inline std::string APInt::ToString(unsigned radix, bool is_signed) const {
  assert(radix == 2 || radix == 8 || radix == 10 || radix == 16);
  if (IsZero()) return "0";

  std::string result;
  bool negative = is_signed && IsNegative();
  APInt tmp = negative ? -(*this) : *this;
  // Use at least 4 bits for radix to avoid truncation (max radix is 16)
  APInt radix_ap(std::max(bit_width_, 4u), static_cast<uint64_t>(radix));
  if (bit_width_ < 4u) {
    tmp = tmp.ZExt(4u);
  }

  while (!tmp.IsZero()) {
    APInt rem = tmp.URem(radix_ap);
    uint64_t digit = rem.GetZExtValue();
    result += (digit < 10 ? char('0' + digit) : char('a' + digit - 10));
    tmp = tmp.UDiv(radix_ap);
  }

  if (negative) result += '-';

  std::reverse(result.begin(), result.end());

  return result;
}

/// \brief Construct an APInt from a string representation, with optional radix
///        prefix.
///
/// The string can be prefixed with:
/// - "0x" or "0X" for hexadecimal (base 16)
/// - "0b" or "0B" for binary (base 2)
/// - "0" for octal (base 8)
///
/// If no prefix is present, the string is parsed as decimal (base 10). The
/// string can also optionally start with a '+' or '-' sign to indicate the sign
/// of the number.
///
/// \param bit_width The bit width of the APInt. Must be at least 1.
/// \param str       The string representation of the value.
inline APInt::APInt(unsigned bit_width, std::string_view str)
    : APInt(bit_width, 0u) {
  if (str.empty()) return;

  unsigned radix = 10;
  bool negative = false;
  size_t start = 0;

  if (str[start] == '-') {
    negative = true;
    ++start;
  } else if (str[start] == '+') {
    ++start;
  }

  if (str.size() > start + 1 && str[start] == '0') {
    if (str[start + 1] == 'x' || str[start + 1] == 'X') {
      radix = 16;
      start += 2;
    } else if (str[start + 1] == 'b' || str[start + 1] == 'B') {
      radix = 2;
      start += 2;
    } else if (str.size() > start + 1) {
      radix = 8;
      ++start;
    }
  }

  APInt radix_ap(bit_width, static_cast<uint64_t>(radix));

  for (size_t i = start; i < str.size(); ++i) {
    char c = str[i];
    uint64_t digit;

    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    } else {
      throw std::invalid_argument("APInt: invalid character in string");
    }

    if (digit >= radix) {
      throw std::invalid_argument("APInt: digit out of range");
    }

    *this *= radix_ap;
    *this += APInt(bit_width, digit);
  }

  if (negative) *this = -(*this);
}

/// \brief A class representing an arbitrary precision signed or unsigned
///        integer, built on top of APInt.
class APSInt : public APInt {
 public:
  /// \brief Default constructor creates an APSInt with a bit width of 1 and a
  ///        value of 0, defaulting to signed.
  explicit APSInt() = default;

  /// \brief Construct a zero APSInt with the specified bit width.
  ///
  /// \param bit_width The bit width of the APSInt. Must be at least 1.
  explicit APSInt(uint32_t bit_width) : APInt(bit_width, 0) {}

  /// \brief Construct a signed APSInt with the specified bit width and value.
  ///
  /// \param bit_width The bit width of the APSInt. Must be at least 1.
  /// \param value     The signed value to encode in two's complement form.
  explicit APSInt(uint32_t bit_width, int64_t value)
      : APInt(bit_width, static_cast<uint64_t>(value)) {}

  /// \brief Construct an APSInt with explicit signedness and raw value bits.
  ///
  /// \param bit_width   The bit width of the APSInt. Must be at least 1.
  /// \param value       The raw value bits.
  /// \param is_unsigned Whether the APSInt should be treated as unsigned.
  explicit APSInt(uint32_t bit_width, uint64_t value, bool is_unsigned)
      : APInt(bit_width, value), is_unsigned_(is_unsigned) {}

  /// \brief Construct an APSInt from an APInt, with optional signedness.
  ///
  /// \param i           The APInt to construct from.
  /// \param is_unsigned Whether the APSInt should be treated as unsigned (true)
  ///                    or signed (false). Defaults to false (signed).
  explicit APSInt(APInt i, bool is_unsigned = false)
      : APInt(std::move(i)), is_unsigned_(is_unsigned) {}

  /// \brief Construct an APSInt from a string representation.
  ///
  /// This constructor interprets the string \p str using the radix of 10.
  /// The interpretation stops at the end of the string. The bit width of the
  /// constructed APSInt is determined automatically.
  ///
  /// \param str the string to be interpreted.
  explicit APSInt(std::string_view str);

  /// \brief Determine sign of this APSInt.
  ///
  /// \returns true if this APSInt is negative; otherwise, false.
  bool IsNegative() const { return IsSigned() && APInt::IsNegative(); }

  /// \brief Determine if this APSInt Value is non-negative (>= 0)
  ///
  /// \returns true if this APSInt is non-negative; otherwise, false.
  bool IsNonNegative() const { return !IsNegative(); }

  /// \brief Determine if this APSInt Value is positive.
  ///
  /// This tests if the value of this APSInt is positive (> 0). Note
  /// that 0 is not a positive value.
  ///
  /// \returns true if this APSInt is positive; otherwise, false.
  bool IsStrictlyPositive() const { return IsNonNegative() && !IsZero(); }

  APSInt &operator=(APInt rhs) {
    // Retain our current sign.
    APInt::operator=(std::move(rhs));
    return *this;
  }

  APSInt &operator=(uint64_t rhs) {
    // Retain our current sign.
    APInt::operator=(APInt(GetBitWidth(), rhs));

    return *this;
  }

  bool Eq(const APSInt &rhs) const {
    return static_cast<const APInt &>(*this) == static_cast<const APInt &>(rhs);
  }

  // Query sign information.
  bool IsSigned() const { return !is_unsigned_; }
  bool IsUnsigned() const { return is_unsigned_; }
  void SetIsUnsigned(bool Val) { is_unsigned_ = Val; }
  void SetIsSigned(bool Val) { is_unsigned_ = !Val; }

  /// Append this APSInt to the specified SmallString.
  // void ToString(std::vector<char> &str, unsigned radix = 10) const {
  //   APInt::ToString(Str, radix, IsSigned());
  // }

  std::string ToString(unsigned radix = 10) const {
    return APInt::ToString(radix, IsSigned());
  }

  /// If this int is representable using an int64_t.
  bool IsRepresentableByInt64() const {
    // For unsigned values with 64 active bits, they technically fit into a
    // int64_t, but the user may get negative numbers and has to manually cast
    // them to unsigned. Let's not bet the user has the sanity to do that and
    // not give them a vague value at the first place.
    return IsSigned() ? IsSignedIntN(64) : IsIntN(63);
  }

  /// Get the correctly-extended \c int64_t value.
  int64_t GetExtValue() const {
    assert(IsRepresentableByInt64() && "Too many bits for int64_t");
    return IsSigned() ? GetSExtValue() : GetZExtValue();
  }

  std::optional<int64_t> TryExtValue() const {
    return IsRepresentableByInt64() ? std::optional<int64_t>(GetExtValue())
                                    : std::nullopt;
  }

  APSInt Trunc(uint32_t width) const {
    return APSInt(APInt::Trunc(width), is_unsigned_);
  }

  APSInt Extend(uint32_t width) const {
    if (is_unsigned_) return APSInt(ZExt(width), is_unsigned_);

    return APSInt(SExt(width), is_unsigned_);
  }

  APSInt ExtOrTrunc(uint32_t width) const {
    if (is_unsigned_) return APSInt(ZExtOrTrunc(width), is_unsigned_);

    return APSInt(SExtOrTrunc(width), is_unsigned_);
  }

  const APSInt &operator%=(const APSInt &rhs) {
    assert(is_unsigned_ == rhs.is_unsigned_ && "Signedness mismatch!");

    if (is_unsigned_) {
      *this = URem(rhs);
    } else {
      *this = SRem(rhs);
    }

    return *this;
  }

  const APSInt &operator/=(const APSInt &rhs) {
    assert(is_unsigned_ == rhs.is_unsigned_ && "Signedness mismatch!");

    if (is_unsigned_) {
      *this = UDiv(rhs);
    } else {
      *this = SDiv(rhs);
    }

    return *this;
  }

  APSInt operator%(const APSInt &rhs) const {
    assert(is_unsigned_ == rhs.is_unsigned_ && "Signedness mismatch!");

    return is_unsigned_ ? APSInt(URem(rhs), true) : APSInt(SRem(rhs), false);
  }

  APSInt operator/(const APSInt &rhs) const {
    assert(is_unsigned_ == rhs.is_unsigned_ && "Signedness mismatch!");
    return is_unsigned_ ? APSInt(UDiv(rhs), true) : APSInt(SDiv(rhs), false);
  }

  APSInt operator>>(unsigned amt) const {
    return is_unsigned_ ? APSInt(LShr(amt), true) : APSInt(AShr(amt), false);
  }

  APSInt &operator>>=(unsigned amt) {
    if (is_unsigned_) {
      LShrInPlace(amt);
    } else {
      AShrInPlace(amt);
    }

    return *this;
  }
  APSInt RelativeShr(unsigned amt) const {
    return is_unsigned_ ? APSInt(RelativeLShr(amt), true)
                        : APSInt(RelativeAShr(amt), false);
  }

  inline bool operator<(const APSInt &rhs) const {
    assert(is_unsigned_ == rhs.is_unsigned_ && "Signedness mismatch!");
    return is_unsigned_ ? ULt(rhs) : SLt(rhs);
  }
  inline bool operator>(const APSInt &rhs) const {
    assert(is_unsigned_ == rhs.is_unsigned_ && "Signedness mismatch!");
    return is_unsigned_ ? UGt(rhs) : SGt(rhs);
  }
  inline bool operator<=(const APSInt &rhs) const {
    assert(is_unsigned_ == rhs.is_unsigned_ && "Signedness mismatch!");
    return is_unsigned_ ? ULe(rhs) : SLe(rhs);
  }
  inline bool operator>=(const APSInt &rhs) const {
    assert(is_unsigned_ == rhs.is_unsigned_ && "Signedness mismatch!");
    return is_unsigned_ ? UGe(rhs) : SGe(rhs);
  }
  inline bool operator==(const APSInt &rhs) const {
    assert(is_unsigned_ == rhs.is_unsigned_ && "Signedness mismatch!");

    return Eq(rhs);
  }

  inline bool operator!=(const APSInt &rhs) const { return !((*this) == rhs); }

  bool operator==(int64_t rhs) const {
    return CompareValues(*this, Get(rhs)) == 0;
  }
  bool operator!=(int64_t rhs) const {
    return CompareValues(*this, Get(rhs)) != 0;
  }
  bool operator<=(int64_t rhs) const {
    return CompareValues(*this, Get(rhs)) <= 0;
  }
  bool operator>=(int64_t rhs) const {
    return CompareValues(*this, Get(rhs)) >= 0;
  }
  bool operator<(int64_t rhs) const {
    return CompareValues(*this, Get(rhs)) < 0;
  }
  bool operator>(int64_t rhs) const {
    return CompareValues(*this, Get(rhs)) > 0;
  }

  // The remaining operators just wrap the logic of APInt, but retain the
  // signedness information.

  APSInt operator<<(unsigned Bits) const {
    return APSInt(static_cast<const APInt &>(*this) << Bits, is_unsigned_);
  }

  APSInt &operator<<=(unsigned amt) {
    static_cast<APInt &>(*this) <<= amt;
    return *this;
  }

  APSInt relativeShl(unsigned amt) const {
    return is_unsigned_ ? APSInt(RelativeLShl(amt), true)
                        : APSInt(RelativeAShl(amt), false);
  }

  APSInt &operator++() {
    ++(static_cast<APInt &>(*this));
    return *this;
  }

  APSInt &operator--() {
    --(static_cast<APInt &>(*this));
    return *this;
  }

  APSInt operator++(int) {
    return APSInt(++static_cast<APInt &>(*this), is_unsigned_);
  }

  APSInt operator--(int) {
    return APSInt(--static_cast<APInt &>(*this), is_unsigned_);
  }

  APSInt operator-() const {
    return APSInt(-static_cast<const APInt &>(*this), is_unsigned_);
  }

  APSInt &operator+=(const APSInt &rhs) {
    assert(is_unsigned_ == rhs.is_unsigned_ && "Signedness mismatch!");
    static_cast<APInt &>(*this) += rhs;
    return *this;
  }

  APSInt &operator-=(const APSInt &rhs) {
    assert(is_unsigned_ == rhs.is_unsigned_ && "Signedness mismatch!");
    static_cast<APInt &>(*this) -= rhs;
    return *this;
  }

  APSInt &operator*=(const APSInt &rhs) {
    assert(is_unsigned_ == rhs.is_unsigned_ && "Signedness mismatch!");
    static_cast<APInt &>(*this) *= rhs;
    return *this;
  }

  APSInt &operator&=(const APSInt &rhs) {
    assert(is_unsigned_ == rhs.is_unsigned_ && "Signedness mismatch!");
    static_cast<APInt &>(*this) &= rhs;
    return *this;
  }

  APSInt &operator|=(const APSInt &rhs) {
    assert(is_unsigned_ == rhs.is_unsigned_ && "Signedness mismatch!");
    static_cast<APInt &>(*this) |= rhs;
    return *this;
  }

  APSInt &operator^=(const APSInt &rhs) {
    assert(is_unsigned_ == rhs.is_unsigned_ && "Signedness mismatch!");
    static_cast<APInt &>(*this) ^= rhs;
    return *this;
  }

  APSInt operator&(const APSInt &rhs) const {
    assert(is_unsigned_ == rhs.is_unsigned_ && "Signedness mismatch!");
    return APSInt(static_cast<const APInt &>(*this) & rhs, is_unsigned_);
  }

  APSInt operator|(const APSInt &rhs) const {
    assert(is_unsigned_ == rhs.is_unsigned_ && "Signedness mismatch!");
    return APSInt(static_cast<const APInt &>(*this) | rhs, is_unsigned_);
  }

  APSInt operator^(const APSInt &rhs) const {
    assert(is_unsigned_ == rhs.is_unsigned_ && "Signedness mismatch!");
    return APSInt(static_cast<const APInt &>(*this) ^ rhs, is_unsigned_);
  }

  APSInt operator*(const APSInt &rhs) const {
    assert(is_unsigned_ == rhs.is_unsigned_ && "Signedness mismatch!");
    return APSInt(static_cast<const APInt &>(*this) * rhs, is_unsigned_);
  }
  APSInt operator+(const APSInt &rhs) const {
    assert(is_unsigned_ == rhs.is_unsigned_ && "Signedness mismatch!");
    return APSInt(static_cast<const APInt &>(*this) + rhs, is_unsigned_);
  }
  APSInt operator-(const APSInt &rhs) const {
    assert(is_unsigned_ == rhs.is_unsigned_ && "Signedness mismatch!");
    return APSInt(static_cast<const APInt &>(*this) - rhs, is_unsigned_);
  }
  APSInt operator~() const {
    return APSInt(~static_cast<const APInt &>(*this), is_unsigned_);
  }

  /// Return the APSInt representing the maximum integer value with the given
  /// bit width and signedness.
  static APSInt GetMaxValue(uint32_t num_bits, bool is_unsigned) {
    return APSInt(is_unsigned ? APInt::GetMaxValue(num_bits)
                              : APInt::GetSignedMaxValue(num_bits),
                  is_unsigned);
  }

  /// Return the APSInt representing the minimum integer value with the given
  /// bit width and signedness.
  static APSInt GetMinValue(uint32_t num_bits, bool is_unsigned) {
    return APSInt(is_unsigned ? APInt::GetMinValue(num_bits)
                              : APInt::GetSignedMinValue(num_bits),
                  is_unsigned);
  }

  /// Determine if two APSInts have the same value, zero- or
  /// sign-extending as needed.
  static bool IsSameValue(const APSInt &i1, const APSInt &i2) {
    return !CompareValues(i1, i2);
  }

  /// Compare underlying values of two numbers.
  static int CompareValues(const APSInt &i1, const APSInt &i2) {
    if (i1.GetBitWidth() == i2.GetBitWidth() && i1.IsSigned() == i2.IsSigned())
      return i1.is_unsigned_ ? i1.Compare(i2) : i1.CompareSigned(i2);

    // Check for a bit-width mismatch.
    if (i1.GetBitWidth() > i2.GetBitWidth())
      return CompareValues(i1, i2.Extend(i1.GetBitWidth()));

    if (i2.GetBitWidth() > i1.GetBitWidth())
      return CompareValues(i1.Extend(i2.GetBitWidth()), i2);

    // We have a signedness mismatch. Check for negative values and do an
    // unsigned compare if both are positive.
    if (i1.IsSigned()) {
      assert(!i2.IsSigned() && "Expected signed mismatch");

      if (i1.IsNegative()) return -1;
    } else {
      assert(i2.IsSigned() && "Expected signed mismatch");

      if (i2.IsNegative()) return 1;
    }

    return i1.Compare(i2);
  }

  static APSInt Get(int64_t x) { return APSInt(APInt(64, x), false); }
  static APSInt GetUnsigned(uint64_t x) { return APSInt(APInt(64, x), true); }

 private:
  bool is_unsigned_ = false;
};

inline bool operator==(int64_t v1, const APSInt &v2) { return v2 == v1; }
inline bool operator!=(int64_t v1, const APSInt &v2) { return v2 != v1; }
inline bool operator<=(int64_t v1, const APSInt &v2) { return v2 >= v1; }
inline bool operator>=(int64_t v1, const APSInt &v2) { return v2 <= v1; }
inline bool operator<(int64_t v1, const APSInt &v2) { return v2 > v1; }
inline bool operator>(int64_t v1, const APSInt &v2) { return v2 < v1; }

}  // namespace bcc
