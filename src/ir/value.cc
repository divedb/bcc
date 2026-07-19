#include "bcc/ir/value.hh"

namespace bcc::ir {

int64_t ConstantInt::GetSExtValue() const noexcept {
  unsigned bits = GetType()->As<IntegerType>()->GetBits();
  if (bits >= 64) return static_cast<int64_t>(value_);
  uint64_t mask = (uint64_t{1} << bits) - 1;
  uint64_t v = value_ & mask;
  if (v & (uint64_t{1} << (bits - 1))) v |= ~mask;
  return static_cast<int64_t>(v);
}

}  // namespace bcc::ir
