#pragma once

#include <cstdint>

#include "bcc/as/mcvalue.hh"
#include "bcc/as/register.hh"

namespace bcc::as {

/// \brief The parsed form of one instruction operand.
enum class OperandKind : uint8_t { kNone, kReg, kImm, kMem };

/// \brief A single parsed operand in AT&T notation.
///
/// * `kReg` — a register (`%rax`).
/// * `kImm` — an immediate (`$expr`); the value is in \ref imm.
/// * `kMem` — a memory reference `disp(base,index,scale)`, possibly
///   rip-relative; fields \ref disp, \ref base, \ref index, \ref scale, \ref
///   rip apply.
///
/// `indirect` records a leading `*` (indirect jump/call target).
struct Operand {
  OperandKind kind = OperandKind::kNone;
  bool indirect = false;

  Reg reg;       ///< kReg
  MCValue imm;   ///< kImm

  MCValue disp;  ///< kMem displacement
  Reg base;      ///< kMem base register (cls kNone if absent)
  Reg index;     ///< kMem index register (cls kNone if absent)
  uint8_t scale = 1;
  bool rip = false;  ///< kMem base is %rip (rip-relative)

  bool IsReg() const noexcept { return kind == OperandKind::kReg; }
  bool IsImm() const noexcept { return kind == OperandKind::kImm; }
  bool IsMem() const noexcept { return kind == OperandKind::kMem; }
};

}  // namespace bcc::as
