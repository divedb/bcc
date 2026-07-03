#pragma once

#include <cstdint>

#include "bcc/as/reloc.hh"

namespace bcc::as {

struct Symbol;

/// \brief A relocatable value: a constant addend plus at most two symbol
///        references and an optional relocation modifier.
///
/// The expression evaluator folds constant subtrees and reduces any surviving
/// symbol references to one of the forms the encoder/ELF layer knows how to
/// lower:
///
///   * pure constant      (`sym == nullptr`)
///   * `sym + addend`
///   * `sym - sym2 + addend`  (a symbol difference, e.g. for `.size`)
struct MCValue {
  int64_t addend = 0;
  Symbol* sym = nullptr;
  Symbol* sym2 = nullptr;
  RelModifier mod = RelModifier::kNone;

  bool IsConstant() const noexcept { return sym == nullptr && sym2 == nullptr; }
  bool IsRelocatable() const noexcept { return !IsConstant(); }

  static MCValue Const(int64_t v) { return MCValue{v, nullptr, nullptr}; }
};

}  // namespace bcc::as
