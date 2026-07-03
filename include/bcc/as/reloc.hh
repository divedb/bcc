#pragma once

#include <cstdint>

namespace bcc::as {

/// \brief x86-64 System V ELF relocation types (`R_X86_64_*`).
///
/// Values match the psABI so they can be written directly into the low 32 bits
/// of an `Elf64_Rela::r_info` field.
enum class RelType : uint32_t {
  kNone = 0,       ///< R_X86_64_NONE
  k64 = 1,         ///< R_X86_64_64      : S + A            (word64)
  kPC32 = 2,       ///< R_X86_64_PC32    : S + A - P        (word32)
  kGOT32 = 3,      ///< R_X86_64_GOT32
  kPLT32 = 4,      ///< R_X86_64_PLT32   : L + A - P        (word32)
  kGOTPCREL = 9,   ///< R_X86_64_GOTPCREL: G + GOT + A - P  (word32)
  k32 = 10,        ///< R_X86_64_32      : S + A            (word32, zero-ext)
  k32S = 11,       ///< R_X86_64_32S     : S + A            (word32, sign-ext)
  k16 = 12,        ///< R_X86_64_16
  kPC16 = 13,      ///< R_X86_64_PC16
  k8 = 14,         ///< R_X86_64_8
  kPC8 = 15,       ///< R_X86_64_PC8
  kPC64 = 24,      ///< R_X86_64_PC64    : S + A - P        (word64)
};

/// \brief A trailing `@modifier` on a symbol reference selecting a relocation
///        flavor, e.g. `foo@PLT`, `foo@GOTPCREL`.
enum class RelModifier : uint8_t {
  kNone = 0,
  kPLT,       ///< @PLT
  kGOTPCREL,  ///< @GOTPCREL
  kGOT,       ///< @GOT
  kGOTOFF,    ///< @GOTOFF
  kTPOFF,     ///< @TPOFF (thread-local, reserved)
};

}  // namespace bcc::as
