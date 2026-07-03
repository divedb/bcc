#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "bcc/as/diagnostic.hh"
#include "bcc/as/mccontext.hh"
#include "bcc/as/operand.hh"

namespace bcc::as {

/// \brief Encodes one x86-64 instruction into the current section's bytes,
///        recording relocations for any symbolic operands.
///
/// The public surface is a single \ref Encode call; the AT&T mnemonic (with its
/// optional size suffix) plus the source-ordered operands select an encoding.
/// All ModRM/SIB/REX/immediate machinery lives in the private helpers.
class Encoder {
 public:
  Encoder(MCContext& ctx, AsmDiag& diag) : ctx_(ctx), diag_(diag) {}

  /// \param mnemonic Instruction mnemonic including any size suffix (e.g.
  ///                 "movq", "add", "jne").
  /// \param ops      Operands in AT&T (source-first) order.
  /// \param loc      Source offset of the mnemonic, for diagnostics.
  /// \return true if an encoding was emitted; false on error (already reported).
  bool Encode(std::string_view mnemonic, std::span<const Operand> ops,
              uint32_t loc);

 private:
  MCContext& ctx_;
  AsmDiag& diag_;
};

}  // namespace bcc::as
