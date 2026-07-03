#pragma once

#include <cstdint>
#include <string_view>

namespace bcc::as {

/// \brief The class of a register, which fixes its operand size and how it is
///        encoded.
enum class RegClass : uint8_t {
  kNone,   ///< absent operand field
  kGpr8,   ///< al, cl, ..., spl/bpl/sil/dil (REX), r8b..r15b
  kGpr8h,  ///< ah, ch, dh, bh (legacy high byte; incompatible with REX)
  kGpr16,  ///< ax, cx, ..., r8w..r15w
  kGpr32,  ///< eax, ecx, ..., r8d..r15d
  kGpr64,  ///< rax, rcx, ..., r8..r15
  kRip,    ///< the instruction pointer, for rip-relative addressing
};

/// \brief A concrete register: its class plus its 0..15 hardware number.
struct Reg {
  RegClass cls = RegClass::kNone;
  uint8_t num = 0;  ///< encoding number 0..15 (low 3 bits + REX extension bit)

  bool Present() const noexcept { return cls != RegClass::kNone; }
  bool IsGpr() const noexcept {
    return cls == RegClass::kGpr8 || cls == RegClass::kGpr8h ||
           cls == RegClass::kGpr16 || cls == RegClass::kGpr32 ||
           cls == RegClass::kGpr64;
  }
  bool IsExtended() const noexcept { return num >= 8; }  // needs a REX ext bit

  /// True for spl/bpl/sil/dil — 8-bit regs that only exist with a REX prefix.
  bool NeedsRex() const noexcept {
    return cls == RegClass::kGpr8 && (num == 4 || num == 5 || num == 6 || num == 7);
  }
  bool IsHigh8() const noexcept { return cls == RegClass::kGpr8h; }

  /// Operand size in bytes (0 for rip/none).
  unsigned SizeBytes() const noexcept {
    switch (cls) {
      case RegClass::kGpr8:
      case RegClass::kGpr8h: return 1;
      case RegClass::kGpr16: return 2;
      case RegClass::kGpr32: return 4;
      case RegClass::kGpr64: return 8;
      default: return 0;
    }
  }
};

/// Looks up a register by its spelling (without the leading `%`).
/// \return true and fills \p out on success; false for an unknown name.
bool LookupRegister(std::string_view name, Reg& out);

}  // namespace bcc::as
