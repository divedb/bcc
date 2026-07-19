#include "bcc/ir/ir_context.hh"

#include <bit>
#include <cassert>

namespace bcc::ir {

namespace {

/// Minimal placeholder classes for the singleton kinds that carry no state
/// beyond their kind.
class SimpleType final : public Type {
 public:
  explicit SimpleType(TypeKind kind) : Type(kind) {}
};

}  // namespace

IRContext::IRContext()
    : void_type_(std::make_unique<SimpleType>(TypeKind::kVoid)),
      i1_(std::make_unique<IntegerType>(1)),
      i8_(std::make_unique<IntegerType>(8)),
      i16_(std::make_unique<IntegerType>(16)),
      i32_(std::make_unique<IntegerType>(32)),
      i64_(std::make_unique<IntegerType>(64)),
      float_type_(std::make_unique<SimpleType>(TypeKind::kFloat)),
      double_type_(std::make_unique<SimpleType>(TypeKind::kDouble)),
      ptr_type_(std::make_unique<SimpleType>(TypeKind::kPointer)) {}

IRContext::~IRContext() = default;

const IntegerType* IRContext::GetIntType(unsigned bits) const noexcept {
  switch (bits) {
    case 1: return i1_.get();
    case 8: return i8_.get();
    case 16: return i16_.get();
    case 32: return i32_.get();
    case 64: return i64_.get();
  }
  assert(false && "unsupported integer width");
  return nullptr;
}

const ArrayType* IRContext::GetArrayType(const Type* element,
                                         uint64_t num_elements) {
  auto& slot = array_types_[{element, num_elements}];
  if (!slot) slot = std::make_unique<ArrayType>(element, num_elements);
  return slot.get();
}

const FunctionType* IRContext::GetFunctionType(
    const Type* ret, std::vector<const Type*> params, bool is_variadic) {
  FunctionTypeKey key{ret, params, is_variadic};
  auto& slot = function_types_[std::move(key)];
  if (!slot) {
    slot = std::make_unique<FunctionType>(ret, std::move(params),
                                          is_variadic);
  }
  return slot.get();
}

StructType* IRContext::CreateStructType(std::string name) {
  unsigned& count = struct_name_counts_[name];
  std::string unique_name =
      count == 0 ? name : name + "." + std::to_string(count - 1);
  ++count;
  auto st = std::make_unique<StructType>(std::move(unique_name),
                                         std::vector<const Type*>{}, 0, 1);
  st->SetOpaque();
  struct_types_.push_back(std::move(st));
  return struct_types_.back().get();
}

const ConstantInt* IRContext::GetInt(const IntegerType* type,
                                     uint64_t value) {
  // Normalize to the type's width so equal values unique to one constant.
  unsigned bits = type->GetBits();
  if (bits < 64) value &= (uint64_t{1} << bits) - 1;
  auto& slot = int_constants_[{type, value}];
  if (!slot) slot = std::make_unique<ConstantInt>(type, value);
  return slot.get();
}

const ConstantFP* IRContext::GetFP(const Type* type, double value) {
  auto& slot = fp_constants_[{type, std::bit_cast<uint64_t>(value)}];
  if (!slot) slot = std::make_unique<ConstantFP>(type, value);
  return slot.get();
}

const ConstantNullPtr* IRContext::GetNullPtr() {
  if (!null_ptr_) {
    null_ptr_ = std::make_unique<ConstantNullPtr>(ptr_type_.get());
  }
  return null_ptr_.get();
}

const ConstantUndef* IRContext::GetUndef(const Type* type) {
  auto& slot = undefs_[type];
  if (!slot) slot = std::make_unique<ConstantUndef>(type);
  return slot.get();
}

const ConstantAggregateZero* IRContext::GetAggregateZero(const Type* type) {
  auto& slot = agg_zeros_[type];
  if (!slot) slot = std::make_unique<ConstantAggregateZero>(type);
  return slot.get();
}

const ConstantString* IRContext::GetString(std::string bytes) {
  auto& slot = strings_[bytes];
  if (!slot) {
    const ArrayType* type = GetArrayType(i8_.get(), bytes.size());
    slot = std::make_unique<ConstantString>(type, std::move(bytes));
  }
  return slot.get();
}

const ConstantAggregate* IRContext::GetAggregate(
    const Type* type, std::vector<const Constant*> elems) {
  aggregates_.push_back(
      std::make_unique<ConstantAggregate>(type, std::move(elems)));
  return aggregates_.back().get();
}

}  // namespace bcc::ir
