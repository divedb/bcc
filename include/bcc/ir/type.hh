#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bcc::ir {

class IRContext;

enum class TypeKind : uint8_t {
  kVoid,
  kInteger,   ///< iN, N in {1, 8, 16, 32, 64}
  kFloat,     ///< IEEE binary32
  kDouble,    ///< IEEE binary64
  kPointer,   ///< opaque `ptr`; pointee types live in load/store/gep operands
  kArray,     ///< [N x T]
  kStruct,    ///< named %struct.X / %union.U with explicit padding fields
  kFunction,  ///< return + params (+ variadic)
};

/// \brief Base of the IR type hierarchy. Types are uniqued by the IRContext,
///        so type equality is pointer equality.
class Type {
 public:
  virtual ~Type() = default;

  Type(const Type&) = delete;
  Type& operator=(const Type&) = delete;

  TypeKind GetKind() const noexcept { return kind_; }

  bool IsVoid() const noexcept { return kind_ == TypeKind::kVoid; }
  bool IsInteger() const noexcept { return kind_ == TypeKind::kInteger; }
  bool IsFloat() const noexcept { return kind_ == TypeKind::kFloat; }
  bool IsDouble() const noexcept { return kind_ == TypeKind::kDouble; }
  bool IsFloatingPoint() const noexcept { return IsFloat() || IsDouble(); }
  bool IsPointer() const noexcept { return kind_ == TypeKind::kPointer; }
  bool IsArray() const noexcept { return kind_ == TypeKind::kArray; }
  bool IsStruct() const noexcept { return kind_ == TypeKind::kStruct; }
  bool IsFunction() const noexcept { return kind_ == TypeKind::kFunction; }
  bool IsAggregate() const noexcept { return IsArray() || IsStruct(); }
  /// Types a load/store/ret/phi/call value may have.
  bool IsFirstClass() const noexcept { return !IsVoid() && !IsFunction(); }

  template <typename T>
  const T* As() const noexcept {
    return T::ClassOf(this) ? static_cast<const T*>(this) : nullptr;
  }

 protected:
  explicit Type(TypeKind kind) : kind_(kind) {}

 private:
  TypeKind kind_;
};

/// \brief iN. Only widths 1, 8, 16, 32 and 64 are created by the context.
class IntegerType final : public Type {
 public:
  explicit IntegerType(unsigned bits)
      : Type(TypeKind::kInteger), bits_(bits) {}

  unsigned GetBits() const noexcept { return bits_; }

  static bool ClassOf(const Type* t) noexcept { return t->IsInteger(); }

 private:
  unsigned bits_;
};

/// \brief [N x T].
class ArrayType final : public Type {
 public:
  ArrayType(const Type* element, uint64_t num_elements)
      : Type(TypeKind::kArray), element_(element),
        num_elements_(num_elements) {}

  const Type* GetElementType() const noexcept { return element_; }
  uint64_t GetNumElements() const noexcept { return num_elements_; }

  static bool ClassOf(const Type* t) noexcept { return t->IsArray(); }

 private:
  const Type* element_;
  uint64_t num_elements_;
};

/// \brief A named struct (%struct.X, %union.U). Fields include explicit
///        padding so LLVM's natural layout reproduces the AST RecordLayout;
///        total size/alignment are copied from the AST and stored here so no
///        layout logic exists in the IR.
class StructType final : public Type {
 public:
  StructType(std::string name, std::vector<const Type*> fields, uint64_t size,
             uint64_t align)
      : Type(TypeKind::kStruct), name_(std::move(name)),
        fields_(std::move(fields)), size_(size), align_(align) {}

  /// The printed name, without the leading '%' (e.g. "struct.S").
  const std::string& GetName() const noexcept { return name_; }
  const std::vector<const Type*>& GetFields() const noexcept {
    return fields_;
  }
  uint64_t GetSize() const noexcept { return size_; }
  uint64_t GetAlign() const noexcept { return align_; }

  /// Fills in the body of a struct created opaque (forward-declared record).
  void SetBody(std::vector<const Type*> fields, uint64_t size,
               uint64_t align) {
    fields_ = std::move(fields);
    size_ = size;
    align_ = align;
    is_opaque_ = false;
  }
  bool IsOpaque() const noexcept { return is_opaque_; }
  void SetOpaque() noexcept { is_opaque_ = true; }

  static bool ClassOf(const Type* t) noexcept { return t->IsStruct(); }

 private:
  std::string name_;
  std::vector<const Type*> fields_;
  uint64_t size_;
  uint64_t align_;
  bool is_opaque_ = false;
};

/// \brief Function type: return type, parameter types, variadic flag.
class FunctionType final : public Type {
 public:
  FunctionType(const Type* ret, std::vector<const Type*> params,
               bool is_variadic)
      : Type(TypeKind::kFunction), ret_(ret), params_(std::move(params)),
        is_variadic_(is_variadic) {}

  const Type* GetReturnType() const noexcept { return ret_; }
  const std::vector<const Type*>& GetParams() const noexcept {
    return params_;
  }
  bool IsVariadic() const noexcept { return is_variadic_; }

  static bool ClassOf(const Type* t) noexcept { return t->IsFunction(); }

 private:
  const Type* ret_;
  std::vector<const Type*> params_;
  bool is_variadic_;
};

}  // namespace bcc::ir
