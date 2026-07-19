#pragma once

#include <cstdint>

#include "bcc/ast/type.hh"
#include "bcc/ir/value.hh"

namespace bcc::codegen {

/// \brief A pointer plus its known alignment (from the AST side — record
///        layout / declared alignment — never from IR types). Everything
///        that names memory flows through this (Clang's Address).
struct Address {
  const ir::Value* ptr = nullptr;
  uint64_t align = 1;

  Address() = default;
  Address(const ir::Value* ptr, uint64_t align) : ptr(ptr), align(align) {}

  bool IsValid() const noexcept { return ptr != nullptr; }
};

/// \brief Where a bit-field's bits live: a naturally-aligned storage unit of
///        \c storage_size bits at byte offset \c storage_offset from the
///        record start, with the field occupying \c width bits starting at
///        bit \c offset (little-endian bit numbering, x86-64).
struct BitFieldInfo {
  uint64_t storage_offset = 0;  ///< bytes from record start, unit-aligned
  unsigned storage_size = 0;    ///< bits: 8/16/32/64
  unsigned offset = 0;          ///< bit offset of the field within the unit
  unsigned width = 0;           ///< bit width of the field
  bool is_signed = false;
};

/// \brief The result of evaluating an expression as a location: an address
///        plus the AST type, or a bit-field reference (address of the
///        storage unit + BitFieldInfo).
class LValue {
 public:
  static LValue MakeAddr(Address addr, QualType type) {
    LValue lv;
    lv.addr_ = addr;
    lv.type_ = type;
    return lv;
  }

  static LValue MakeBitField(Address storage_addr, QualType type,
                             const BitFieldInfo* bf) {
    LValue lv;
    lv.addr_ = storage_addr;
    lv.type_ = type;
    lv.bf_ = bf;
    return lv;
  }

  Address GetAddress() const noexcept { return addr_; }
  const ir::Value* GetPointer() const noexcept { return addr_.ptr; }
  uint64_t GetAlign() const noexcept { return addr_.align; }
  QualType GetType() const noexcept { return type_; }

  bool IsBitField() const noexcept { return bf_ != nullptr; }
  const BitFieldInfo& GetBitFieldInfo() const noexcept { return *bf_; }

 private:
  Address addr_;
  QualType type_;
  const BitFieldInfo* bf_ = nullptr;
};

/// \brief The result of evaluating an expression as a value: a scalar
///        ir::Value, or — for aggregates — the address of the memory
///        holding the value (aggregates never exist as free-standing IR
///        values during expression evaluation).
class RValue {
 public:
  static RValue Get(const ir::Value* v) {
    RValue rv;
    rv.scalar_ = v;
    return rv;
  }

  static RValue GetAggregate(Address addr) {
    RValue rv;
    rv.aggregate_ = addr;
    return rv;
  }

  bool IsScalar() const noexcept { return !aggregate_.IsValid(); }
  bool IsAggregate() const noexcept { return aggregate_.IsValid(); }

  /// Null for void expressions.
  const ir::Value* GetScalarValue() const noexcept { return scalar_; }
  Address GetAggregateAddress() const noexcept { return aggregate_; }

 private:
  const ir::Value* scalar_ = nullptr;
  Address aggregate_;
};

}  // namespace bcc::codegen
