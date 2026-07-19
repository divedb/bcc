#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "bcc/ir/type.hh"
#include "bcc/ir/value.hh"

namespace bcc::ir {

/// \brief Owns and uniques all IR types and simple constants, so equality on
///        them is pointer equality (mirrors ASTContext's role for canonical
///        types). One per Module.
class IRContext {
 public:
  IRContext();

  IRContext(const IRContext&) = delete;
  IRContext& operator=(const IRContext&) = delete;

  ~IRContext();

  //===--------------------------------------------------------------------===//
  // Types.
  //===--------------------------------------------------------------------===//

  const Type* GetVoidType() const noexcept { return void_type_.get(); }
  const IntegerType* GetInt1Type() const noexcept { return i1_.get(); }
  const IntegerType* GetInt8Type() const noexcept { return i8_.get(); }
  const IntegerType* GetInt16Type() const noexcept { return i16_.get(); }
  const IntegerType* GetInt32Type() const noexcept { return i32_.get(); }
  const IntegerType* GetInt64Type() const noexcept { return i64_.get(); }
  const Type* GetFloatType() const noexcept { return float_type_.get(); }
  const Type* GetDoubleType() const noexcept { return double_type_.get(); }
  const Type* GetPointerType() const noexcept { return ptr_type_.get(); }

  /// iN for N in {1, 8, 16, 32, 64}.
  const IntegerType* GetIntType(unsigned bits) const noexcept;

  const ArrayType* GetArrayType(const Type* element, uint64_t num_elements);
  const FunctionType* GetFunctionType(const Type* ret,
                                      std::vector<const Type*> params,
                                      bool is_variadic);

  /// Creates a new named struct with no body yet (fill via SetBody). The
  /// name is made unique within this context ("struct.S", "struct.S.0", ...).
  StructType* CreateStructType(std::string name);

  /// All named structs, in creation order (the printer's definition order).
  const std::vector<std::unique_ptr<StructType>>& GetStructTypes()
      const noexcept {
    return struct_types_;
  }

  //===--------------------------------------------------------------------===//
  // Constants.
  //===--------------------------------------------------------------------===//

  const ConstantInt* GetInt(const IntegerType* type, uint64_t value);
  const ConstantInt* GetInt1(bool value) { return GetInt(i1_.get(), value); }
  const ConstantInt* GetInt8(uint64_t v) { return GetInt(i8_.get(), v); }
  const ConstantInt* GetInt32(uint64_t v) { return GetInt(i32_.get(), v); }
  const ConstantInt* GetInt64(uint64_t v) { return GetInt(i64_.get(), v); }

  const ConstantFP* GetFP(const Type* type, double value);
  const ConstantNullPtr* GetNullPtr();
  const ConstantUndef* GetUndef(const Type* type);
  const ConstantAggregateZero* GetAggregateZero(const Type* type);

  /// [bytes.size() x i8] byte-array constant; \p bytes should already
  /// include any trailing NUL.
  const ConstantString* GetString(std::string bytes);

  /// Not uniqued (owned only): explicit aggregate initializer.
  const ConstantAggregate* GetAggregate(const Type* type,
                                        std::vector<const Constant*> elems);

 private:
  std::unique_ptr<Type> void_type_;
  std::unique_ptr<IntegerType> i1_, i8_, i16_, i32_, i64_;
  std::unique_ptr<Type> float_type_, double_type_, ptr_type_;

  std::map<std::pair<const Type*, uint64_t>, std::unique_ptr<ArrayType>>
      array_types_;

  struct FunctionTypeKey {
    const Type* ret;
    std::vector<const Type*> params;
    bool variadic;
    bool operator<(const FunctionTypeKey& o) const noexcept {
      return std::tie(ret, params, variadic) <
             std::tie(o.ret, o.params, o.variadic);
    }
  };
  std::map<FunctionTypeKey, std::unique_ptr<FunctionType>> function_types_;

  std::vector<std::unique_ptr<StructType>> struct_types_;
  std::map<std::string, unsigned> struct_name_counts_;

  std::map<std::pair<const IntegerType*, uint64_t>,
           std::unique_ptr<ConstantInt>>
      int_constants_;
  std::map<std::pair<const Type*, uint64_t>, std::unique_ptr<ConstantFP>>
      fp_constants_;
  std::unique_ptr<ConstantNullPtr> null_ptr_;
  std::map<const Type*, std::unique_ptr<ConstantUndef>> undefs_;
  std::map<const Type*, std::unique_ptr<ConstantAggregateZero>> agg_zeros_;
  std::map<std::string, std::unique_ptr<ConstantString>> strings_;
  std::vector<std::unique_ptr<ConstantAggregate>> aggregates_;
};

}  // namespace bcc::ir
