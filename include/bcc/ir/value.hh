#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "bcc/ir/type.hh"

namespace bcc::ir {

enum class ValueKind : uint8_t {
  kArgument,
  kBasicBlock,
  kInstruction,
  // Constants (contiguous range, see Value::IsConstant).
  kConstantInt,
  kConstantFP,
  kConstantNullPtr,
  kConstantUndef,
  kConstantAggregateZero,
  kConstantString,
  kConstantAggregate,
  kGlobalVariable,
  kFunction,
};

/// \brief Base of everything an instruction can reference. Values are
///        anonymous; \c name_ is only a printing hint (the printer's slot
///        tracker assigns %0, %1, ... and de-duplicates hints).
class Value {
 public:
  virtual ~Value() = default;

  Value(const Value&) = delete;
  Value& operator=(const Value&) = delete;

  ValueKind GetValueKind() const noexcept { return value_kind_; }
  const Type* GetType() const noexcept { return type_; }

  const std::string& GetName() const noexcept { return name_; }
  void SetName(std::string name) { name_ = std::move(name); }
  bool HasName() const noexcept { return !name_.empty(); }

  bool IsConstant() const noexcept {
    return value_kind_ >= ValueKind::kConstantInt;
  }

  template <typename T>
  const T* As() const noexcept {
    return T::ClassOf(this) ? static_cast<const T*>(this) : nullptr;
  }
  template <typename T>
  T* As() noexcept {
    return T::ClassOf(this) ? static_cast<T*>(this) : nullptr;
  }

 protected:
  Value(ValueKind kind, const Type* type)
      : value_kind_(kind), type_(type) {}

 private:
  ValueKind value_kind_;
  const Type* type_;
  std::string name_;
};

class Function;

/// \brief A formal parameter of a function definition.
class Argument final : public Value {
 public:
  Argument(const Type* type, Function* parent, unsigned index)
      : Value(ValueKind::kArgument, type), parent_(parent), index_(index) {}

  Function* GetParent() const noexcept { return parent_; }
  unsigned GetIndex() const noexcept { return index_; }

  static bool ClassOf(const Value* v) noexcept {
    return v->GetValueKind() == ValueKind::kArgument;
  }

 private:
  Function* parent_;
  unsigned index_;
};

//===----------------------------------------------------------------------===//
// Constants. All constants are created and uniqued (where meaningful) by the
// IRContext, so simple-constant equality is pointer equality.
//===----------------------------------------------------------------------===//

class Constant : public Value {
 public:
  static bool ClassOf(const Value* v) noexcept { return v->IsConstant(); }

 protected:
  Constant(ValueKind kind, const Type* type) : Value(kind, type) {}
};

/// \brief An integer constant. The value is stored as the sign-extended
///        two's-complement bit pattern and printed as a signed decimal
///        (i1 prints as true/false), matching LLVM's writer.
class ConstantInt final : public Constant {
 public:
  ConstantInt(const IntegerType* type, uint64_t value)
      : Constant(ValueKind::kConstantInt, type), value_(value) {}

  /// Raw 64-bit pattern (low GetBits() bits are significant).
  uint64_t GetValue() const noexcept { return value_; }
  /// The value sign-extended from the type's width.
  int64_t GetSExtValue() const noexcept;
  bool IsZero() const noexcept { return GetSExtValue() == 0; }

  static bool ClassOf(const Value* v) noexcept {
    return v->GetValueKind() == ValueKind::kConstantInt;
  }

 private:
  uint64_t value_;
};

/// \brief A float/double constant, stored as double (float-typed constants
///        hold a value exactly representable in float).
class ConstantFP final : public Constant {
 public:
  ConstantFP(const Type* type, double value)
      : Constant(ValueKind::kConstantFP, type), value_(value) {}

  double GetValue() const noexcept { return value_; }

  static bool ClassOf(const Value* v) noexcept {
    return v->GetValueKind() == ValueKind::kConstantFP;
  }

 private:
  double value_;
};

/// \brief The `null` pointer constant.
class ConstantNullPtr final : public Constant {
 public:
  explicit ConstantNullPtr(const Type* ptr_type)
      : Constant(ValueKind::kConstantNullPtr, ptr_type) {}

  static bool ClassOf(const Value* v) noexcept {
    return v->GetValueKind() == ValueKind::kConstantNullPtr;
  }
};

/// \brief `undef` of any first-class type.
class ConstantUndef final : public Constant {
 public:
  explicit ConstantUndef(const Type* type)
      : Constant(ValueKind::kConstantUndef, type) {}

  static bool ClassOf(const Value* v) noexcept {
    return v->GetValueKind() == ValueKind::kConstantUndef;
  }
};

/// \brief `zeroinitializer` for an aggregate type.
class ConstantAggregateZero final : public Constant {
 public:
  explicit ConstantAggregateZero(const Type* type)
      : Constant(ValueKind::kConstantAggregateZero, type) {}

  static bool ClassOf(const Value* v) noexcept {
    return v->GetValueKind() == ValueKind::kConstantAggregateZero;
  }
};

/// \brief A byte-array constant printed as c"..."; the bytes include any
///        trailing NUL. Type is [bytes.size() x i8].
class ConstantString final : public Constant {
 public:
  ConstantString(const ArrayType* type, std::string bytes)
      : Constant(ValueKind::kConstantString, type),
        bytes_(std::move(bytes)) {}

  const std::string& GetBytes() const noexcept { return bytes_; }

  static bool ClassOf(const Value* v) noexcept {
    return v->GetValueKind() == ValueKind::kConstantString;
  }

 private:
  std::string bytes_;
};

/// \brief A constant array or struct with explicit element list (one per IR
///        field/element, padding included for structs).
class ConstantAggregate final : public Constant {
 public:
  ConstantAggregate(const Type* type, std::vector<const Constant*> elements)
      : Constant(ValueKind::kConstantAggregate, type),
        elements_(std::move(elements)) {}

  const std::vector<const Constant*>& GetElements() const noexcept {
    return elements_;
  }

  static bool ClassOf(const Value* v) noexcept {
    return v->GetValueKind() == ValueKind::kConstantAggregate;
  }

 private:
  std::vector<const Constant*> elements_;
};

enum class Linkage : uint8_t {
  kExternal,  ///< default: visible to the linker
  kInternal,  ///< `static` at file scope, static locals
  kPrivate,   ///< string literals and other unnamed module-local data
};

/// \brief A named module-level entity; as a Value it is the entity's
///        *address*, so its type is `ptr`.
class GlobalValue : public Constant {
 public:
  Linkage GetLinkage() const noexcept { return linkage_; }
  void SetLinkage(Linkage linkage) noexcept { linkage_ = linkage; }

  static bool ClassOf(const Value* v) noexcept {
    return v->GetValueKind() == ValueKind::kGlobalVariable ||
           v->GetValueKind() == ValueKind::kFunction;
  }

 protected:
  GlobalValue(ValueKind kind, const Type* ptr_type, std::string name,
              Linkage linkage)
      : Constant(kind, ptr_type), linkage_(linkage) {
    SetName(std::move(name));
  }

 private:
  Linkage linkage_;
};

/// \brief A module-level variable. A null initializer means this is only a
///        declaration (`external global`).
class GlobalVariable final : public GlobalValue {
 public:
  GlobalVariable(const Type* ptr_type, std::string name,
                 const Type* value_type, const Constant* initializer,
                 Linkage linkage, bool is_const, uint64_t align)
      : GlobalValue(ValueKind::kGlobalVariable, ptr_type, std::move(name),
                    linkage),
        value_type_(value_type), initializer_(initializer),
        is_const_(is_const), align_(align) {}

  const Type* GetValueType() const noexcept { return value_type_; }
  const Constant* GetInitializer() const noexcept { return initializer_; }
  void SetInitializer(const Constant* init) noexcept { initializer_ = init; }
  bool IsDeclaration() const noexcept { return initializer_ == nullptr; }
  bool IsConst() const noexcept { return is_const_; }
  uint64_t GetAlign() const noexcept { return align_; }

  /// String literals print as `private unnamed_addr constant`.
  bool IsUnnamedAddr() const noexcept { return is_unnamed_addr_; }
  void SetUnnamedAddr(bool v) noexcept { is_unnamed_addr_ = v; }

  static bool ClassOf(const Value* v) noexcept {
    return v->GetValueKind() == ValueKind::kGlobalVariable;
  }

 private:
  const Type* value_type_;
  const Constant* initializer_;
  bool is_const_;
  bool is_unnamed_addr_ = false;
  uint64_t align_;
};

}  // namespace bcc::ir
