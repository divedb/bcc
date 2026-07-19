#pragma once

#include <memory>

#include "bcc/support/apfloat.hh"
#include "bcc/support/apint.hh"

namespace bcc {

class VarDecl;
class FunctionDecl;
class StringLiteral;
class CompoundLiteralExpr;

/// APValue - This class implements a discriminated union of compile-time
/// constant values, mirroring what Clang uses internally for constant
/// expression evaluation. It can hold:
///
///   None          – uninitialized / error sentinel
///   Int           – arbitrary-precision signed integer  (APSInt)
///   Float         – arbitrary-precision floating point  (APFloat)
///   ComplexInt    – complex<APSInt>
///   ComplexFloat  – complex<APFloat>
///   Vector        – fixed-length vector of APValues
///   Array         – constant array (elements + optional filler)
///   Struct        – record with base-class + field sub-values
///   Union         – tagged union (active field index + value)
///   AddrLabelDiff – GNU &&label1 - &&label2
///
/// Note: LValue and MemberPointer are intentionally omitted here because
/// they require AST node pointers that belong to the compiler front-end.
/// Add them when you integrate APValue with your AST.
class APValue {
 public:
  enum class ValueKind : uint8_t {
    kNone,
    kInt,
    kFloat,
    kComplexInt,
    kComplexFloat,
    kVector,
    kArray,
    kStruct,
    kUnion,
    kAddrLabelDiff,
    kLValue,
  };

  struct ComplexIntVal {
    APSInt real;
    APSInt imag;

    ComplexIntVal(APSInt r, APSInt i)
        : real(std::move(r)), imag(std::move(i)) {}
  };

  struct ComplexFloatVal {
    APFloat real;
    APFloat imag;

    ComplexFloatVal(APFloat r, APFloat i)
        : real(std::move(r)), imag(std::move(i)) {}
  };

  struct VectorVal {
    std::vector<APValue> elems;
    explicit VectorVal(std::vector<APValue> e) : elems(std::move(e)) {}
  };

  /// Constant array. Elements may be individually initialized; the remainder
  /// share a single "filler" element (common for zero-initialized arrays).
  struct ArrayVal {
    std::vector<APValue> elems;       ///< explicitly initialized elements
    std::unique_ptr<APValue> filler;  ///< value for remaining positions
    unsigned total_size = 0;

    ArrayVal() = default;
    ArrayVal(unsigned total, APValue fill, std::vector<APValue> init = {})
        : elems(std::move(init)),
          filler(std::make_unique<APValue>(std::move(fill))),
          total_size(total) {}

    // Deep-copy support
    ArrayVal(const ArrayVal& o)
        : elems(o.elems),
          filler(o.filler ? std::make_unique<APValue>(*o.filler) : nullptr),
          total_size(o.total_size) {}
    ArrayVal& operator=(const ArrayVal& o) {
      elems = o.elems;
      filler = o.filler ? std::make_unique<APValue>(*o.filler) : nullptr;
      total_size = o.total_size;

      return *this;
    }

    ArrayVal(ArrayVal&&) = default;
    ArrayVal& operator=(ArrayVal&&) = default;
  };

  /// Struct/class value: a flat list of base sub-objects followed by fields.
  struct StructVal {
    std::vector<APValue> bases;
    std::vector<APValue> fields;

    StructVal() = default;
    StructVal(std::vector<APValue> b, std::vector<APValue> f)
        : bases(std::move(b)), fields(std::move(f)) {}
  };

  /// Union value: which field is active (index into the record's field list)
  /// and what value it holds.  field_index == UINT_MAX means no active field.
  struct UnionVal {
    unsigned field_index = ~0u;
    std::unique_ptr<APValue> value;

    UnionVal() = default;
    UnionVal(unsigned idx, APValue v)
        : field_index(idx), value(std::make_unique<APValue>(std::move(v))) {}

    UnionVal(const UnionVal& o)
        : field_index(o.field_index),
          value(o.value ? std::make_unique<APValue>(*o.value) : nullptr) {}

    UnionVal& operator=(const UnionVal& o) {
      field_index = o.field_index;
      value = o.value ? std::make_unique<APValue>(*o.value) : nullptr;
      return *this;
    }

    UnionVal(UnionVal&&) = default;
    UnionVal& operator=(UnionVal&&) = default;
  };

  /// GNU extension: &&label1 - &&label2 (difference of label addresses).
  /// We store the labels as opaque integer IDs; your front-end can map those
  /// to AST nodes.
  struct AddrLabelDiffVal {
    uint64_t lhsLabelId = 0;
    uint64_t rhsLabelId = 0;
    AddrLabelDiffVal(uint64_t l, uint64_t r) : lhsLabelId(l), rhsLabelId(r) {}
  };

  /// \brief LValue represents an address constant, which can be a pointer to a
  ///        global variable, a string literal, or a null pointer. It consists
  ///        of a base (the address being pointed to) and an optional offset
  ///        (for pointer arithmetic).
  struct LValueVal {
    struct GlobalVar {
      const VarDecl* decl;

      bool operator==(const GlobalVar& o) const { return decl == o.decl; }
      bool operator!=(const GlobalVar& o) const { return !(*this == o); }
    };

    struct Function {
      const FunctionDecl* decl;

      bool operator==(const Function& o) const { return decl == o.decl; }
      bool operator!=(const Function& o) const { return !(*this == o); }
    };

    struct StringLit {
      const StringLiteral* expr;

      bool operator==(const StringLit& o) const { return expr == o.expr; }
      bool operator!=(const StringLit& o) const { return !(*this == o); }
    };

    struct CompoundLiteral {
      const CompoundLiteralExpr* expr;

      bool operator==(const CompoundLiteral& o) const { return expr == o.expr; }
      bool operator!=(const CompoundLiteral& o) const { return !(*this == o); }
    };

    struct NullPtr {
      bool operator==(const NullPtr&) const { return true; }
      bool operator!=(const NullPtr&) const { return false; }
    };

    static LValueVal MakeGlobalVar(const VarDecl* decl, int64_t offset = 0) {
      LValueVal v;
      v.base = GlobalVar{decl};
      v.offset = offset;

      return v;
    }

    static LValueVal MakeStringLit(const StringLiteral* expr,
                                   int64_t offset = 0) {
      LValueVal v;
      v.base = StringLit{expr};
      v.offset = offset;

      return v;
    }

    static LValueVal MakeFunction(const FunctionDecl* decl,
                                  int64_t offset = 0) {
      LValueVal v;
      v.base = Function{decl};
      v.offset = offset;

      return v;
    }

    static LValueVal MakeCompoundLiteral(const CompoundLiteralExpr* expr,
                                         int64_t offset = 0) {
      LValueVal v;
      v.base = CompoundLiteral{expr};
      v.offset = offset;

      return v;
    }

    static LValueVal MakeNullPtr(int64_t offset = 0) {
      LValueVal v;
      v.base = NullPtr{};
      v.offset = offset;

      return v;
    }

    using Base =
        std::variant<NullPtr, GlobalVar, Function, StringLit, CompoundLiteral>;

    Base base;
    int64_t offset = 0;

    bool IsNullPointer() const { return std::holds_alternative<NullPtr>(base); }

    bool operator==(const LValueVal& o) const {
      return base == o.base && offset == o.offset;
    }
  };

 private:
  // -----------------------------------------------------------------------
  // Storage
  // -----------------------------------------------------------------------
  using Storage = std::variant<std::monostate,    // None
                               APSInt,            // Int
                               APFloat,           // Float
                               ComplexIntVal,     // ComplexInt
                               ComplexFloatVal,   // ComplexFloat
                               VectorVal,         // Vector
                               ArrayVal,          // Array
                               StructVal,         // Struct
                               UnionVal,          // Union
                               AddrLabelDiffVal,  // AddrLabelDiff
                               LValueVal          // LValue
                               >;

  Storage data_;

  static ValueKind KindFromIndex(std::size_t i) noexcept {
    static constexpr ValueKind table[] = {
        ValueKind::kNone,          ValueKind::kInt,          ValueKind::kFloat,
        ValueKind::kComplexInt,    ValueKind::kComplexFloat, ValueKind::kVector,
        ValueKind::kArray,         ValueKind::kStruct,       ValueKind::kUnion,
        ValueKind::kAddrLabelDiff, ValueKind::kLValue,
    };

    return table[i];
  }

 public:
  APValue() = default;
  explicit APValue(APSInt v) : data_(std::move(v)) {}
  explicit APValue(APInt v) : data_(APSInt(std::move(v))) {}
  explicit APValue(APFloat v) : data_(std::move(v)) {}
  APValue(APSInt real, APSInt imag)
      : data_(ComplexIntVal(std::move(real), std::move(imag))) {}
  APValue(APFloat real, APFloat imag)
      : data_(ComplexFloatVal(std::move(real), std::move(imag))) {}

  static APValue MakeVector(std::vector<APValue> elems) {
    APValue v;
    v.data_ = VectorVal(std::move(elems));

    return v;
  }

  static APValue MakeArray(unsigned total_size, APValue filler,
                           std::vector<APValue> initElems = {}) {
    APValue v;
    v.data_ = ArrayVal(total_size, std::move(filler), std::move(initElems));

    return v;
  }

  static APValue MakeStruct(std::vector<APValue> bases,
                            std::vector<APValue> fields) {
    APValue v;
    v.data_ = StructVal(std::move(bases), std::move(fields));
    return v;
  }

  static APValue MakeUnion(unsigned field_index, APValue fieldVal) {
    APValue v;
    v.data_ = UnionVal(field_index, std::move(fieldVal));

    return v;
  }

  static APValue MakeAddrLabelDiff(uint64_t lhs, uint64_t rhs) {
    APValue v;
    v.data_ = AddrLabelDiffVal(lhs, rhs);

    return v;
  }

  static APValue MakeLValue(const VarDecl* var_decl, int64_t offset = 0) {
    APValue v;
    v.data_ = LValueVal::MakeGlobalVar(var_decl, offset);

    return v;
  }

  static APValue MakeLValue(const StringLiteral* str_expr, int64_t offset = 0) {
    APValue v;
    v.data_ = LValueVal::MakeStringLit(str_expr, offset);

    return v;
  }

  static APValue MakeLValue(const FunctionDecl* fn_decl, int64_t offset = 0) {
    APValue v;
    v.data_ = LValueVal::MakeFunction(fn_decl, offset);

    return v;
  }

  static APValue MakeLValue(const CompoundLiteralExpr* expr,
                            int64_t offset = 0) {
    APValue v;
    v.data_ = LValueVal::MakeCompoundLiteral(expr, offset);

    return v;
  }

  static APValue MakeLValue(int64_t offset = 0) {
    APValue v;
    v.data_ = LValueVal::MakeNullPtr(offset);

    return v;
  }

  APValue(const APValue&) = default;
  APValue(APValue&&) = default;
  APValue& operator=(const APValue&) = default;
  APValue& operator=(APValue&&) = default;

  ~APValue() = default;

  ValueKind GetKind() const noexcept { return KindFromIndex(data_.index()); }

  bool IsNone() const noexcept { return data_.index() == 0; }
  bool IsInt() const noexcept { return data_.index() == 1; }
  bool IsFloat() const noexcept { return data_.index() == 2; }
  bool IsComplexInt() const noexcept { return data_.index() == 3; }
  bool IsComplexFloat() const noexcept { return data_.index() == 4; }
  bool IsVector() const noexcept { return data_.index() == 5; }
  bool IsArray() const noexcept { return data_.index() == 6; }
  bool IsStruct() const noexcept { return data_.index() == 7; }
  bool IsUnion() const noexcept { return data_.index() == 8; }
  bool IsAddrLabelDiff() const noexcept { return data_.index() == 9; }

  const APSInt& GetInt() const {
    assert(IsInt() && "APValue is not Int");

    return std::get<APSInt>(data_);
  }
  APSInt& GetInt() {
    assert(IsInt() && "APValue is not Int");

    return std::get<APSInt>(data_);
  }

  const APFloat& GetFloat() const {
    assert(IsFloat() && "APValue is not Float");

    return std::get<APFloat>(data_);
  }

  APFloat& GetFloat() {
    assert(IsFloat() && "APValue is not Float");

    return std::get<APFloat>(data_);
  }

  const APSInt& GetComplexIntReal() const {
    assert(IsComplexInt() && "APValue is not ComplexInt");

    return std::get<ComplexIntVal>(data_).real;
  }

  const APSInt& GetComplexIntImag() const {
    assert(IsComplexInt() && "APValue is not ComplexInt");

    return std::get<ComplexIntVal>(data_).imag;
  }
  APSInt& GetComplexIntReal() { return std::get<ComplexIntVal>(data_).real; }
  APSInt& GetComplexIntImag() { return std::get<ComplexIntVal>(data_).imag; }

  const APFloat& GetComplexFloatReal() const {
    assert(IsComplexFloat() && "APValue is not ComplexFloat");

    return std::get<ComplexFloatVal>(data_).real;
  }
  const APFloat& GetComplexFloatImag() const {
    assert(IsComplexFloat() && "APValue is not ComplexFloat");

    return std::get<ComplexFloatVal>(data_).imag;
  }

  APFloat& GetComplexFloatReal() {
    return std::get<ComplexFloatVal>(data_).real;
  }

  APFloat& GetComplexFloatImag() {
    return std::get<ComplexFloatVal>(data_).imag;
  }

  unsigned GetVectorLength() const {
    assert(IsVector() && "APValue is not Vector");

    return static_cast<unsigned>(std::get<VectorVal>(data_).elems.size());
  }

  const APValue& GetVectorElt(unsigned i) const {
    assert(IsVector());

    return std::get<VectorVal>(data_).elems.at(i);
  }

  APValue& GetVectorElt(unsigned i) {
    assert(IsVector());

    return std::get<VectorVal>(data_).elems.at(i);
  }

  unsigned GetArraySize() const {
    assert(IsArray() && "APValue is not Array");

    return std::get<ArrayVal>(data_).total_size;
  }

  unsigned GetArrayInitializedElts() const {
    assert(IsArray());
    return static_cast<unsigned>(std::get<ArrayVal>(data_).elems.size());
  }

  bool HasArrayFiller() const {
    assert(IsArray());
    return std::get<ArrayVal>(data_).filler != nullptr;
  }

  const APValue& GetArrayFiller() const {
    assert(IsArray() && HasArrayFiller() && "No filler");
    return *std::get<ArrayVal>(data_).filler;
  }

  APValue& GetArrayFiller() {
    assert(IsArray() && HasArrayFiller());
    return *std::get<ArrayVal>(data_).filler;
  }

  const APValue& GetArrayInitializedElt(unsigned i) const {
    assert(IsArray());
    return std::get<ArrayVal>(data_).elems.at(i);
  }

  APValue& GetArrayInitializedElt(unsigned i) {
    assert(IsArray());
    return std::get<ArrayVal>(data_).elems.at(i);
  }

  const APValue& GetArrayElement(unsigned i) const {
    assert(IsArray());

    const auto& a = std::get<ArrayVal>(data_);

    if (i < a.elems.size()) return a.elems[i];

    assert(a.filler && "No filler for uninitialized element");

    return *a.filler;
  }

  unsigned GetStructNumBases() const {
    assert(IsStruct() && "APValue is not Struct");

    return static_cast<unsigned>(std::get<StructVal>(data_).bases.size());
  }

  unsigned GetStructNumFields() const {
    assert(IsStruct());

    return static_cast<unsigned>(std::get<StructVal>(data_).fields.size());
  }

  const APValue& GetStructBase(unsigned i) const {
    assert(IsStruct());
    return std::get<StructVal>(data_).bases.at(i);
  }

  APValue& GetStructBase(unsigned i) {
    assert(IsStruct());
    return std::get<StructVal>(data_).bases.at(i);
  }

  const APValue& GetStructField(unsigned i) const {
    assert(IsStruct());
    return std::get<StructVal>(data_).fields.at(i);
  }

  APValue& GetStructField(unsigned i) {
    assert(IsStruct());
    return std::get<StructVal>(data_).fields.at(i);
  }

  unsigned GetUnionFieldIndex() const {
    assert(IsUnion() && "APValue is not Union");
    return std::get<UnionVal>(data_).field_index;
  }

  bool HasUnionValue() const {
    assert(IsUnion());

    return std::get<UnionVal>(data_).value != nullptr;
  }

  const APValue& GetUnionValue() const {
    assert(IsUnion() && HasUnionValue());
    return *std::get<UnionVal>(data_).value;
  }

  APValue& GetUnionValue() {
    assert(IsUnion() && HasUnionValue());

    return *std::get<UnionVal>(data_).value;
  }

  uint64_t GetAddrLabelDiffLHS() const {
    assert(IsAddrLabelDiff());

    return std::get<AddrLabelDiffVal>(data_).lhsLabelId;
  }

  uint64_t GetAddrLabelDiffRHS() const {
    assert(IsAddrLabelDiff());

    return std::get<AddrLabelDiffVal>(data_).rhsLabelId;
  }

  bool IsLValue() const noexcept {
    return std::holds_alternative<LValueVal>(data_);
  }

  bool IsNullPointer() const noexcept {
    return IsLValue() && std::get<LValueVal>(data_).IsNullPointer();
  }

  const LValueVal& GetLValue() const {
    assert(IsLValue() && "APValue is not LValue");

    return std::get<LValueVal>(data_);
  }

  LValueVal& GetLValue() {
    assert(IsLValue() && "APValue is not LValue");

    return std::get<LValueVal>(data_);
  }

  void SetInt(APSInt v) { data_ = std::move(v); }
  void SetFloat(APFloat v) { data_ = std::move(v); }

  void SetComplex(APSInt r, APSInt i) {
    data_ = ComplexIntVal(std::move(r), std::move(i));
  }

  void SetComplex(APFloat r, APFloat i) {
    data_ = ComplexFloatVal(std::move(r), std::move(i));
  }

  void SetNone() { data_ = std::monostate{}; }

  bool operator==(const APValue& rhs) const {
    if (GetKind() != rhs.GetKind()) return false;
    switch (GetKind()) {
      case ValueKind::kNone:
        return true;

      case ValueKind::kInt:
        return GetInt() == rhs.GetInt();

      case ValueKind::kFloat:
        return GetFloat() == rhs.GetFloat();

      case ValueKind::kComplexInt:
        return GetComplexIntReal() == rhs.GetComplexIntReal() &&
               GetComplexIntImag() == rhs.GetComplexIntImag();

      case ValueKind::kComplexFloat:
        return GetComplexFloatReal() == rhs.GetComplexFloatReal() &&
               GetComplexFloatImag() == rhs.GetComplexFloatImag();

      case ValueKind::kVector: {
        if (GetVectorLength() != rhs.GetVectorLength()) return false;
        for (unsigned i = 0, n = GetVectorLength(); i < n; ++i)
          if (!(GetVectorElt(i) == rhs.GetVectorElt(i))) return false;
        return true;
      }

      case ValueKind::kArray: {
        if (GetArraySize() != rhs.GetArraySize()) return false;
        for (unsigned i = 0, n = GetArraySize(); i < n; ++i)
          if (!(GetArrayElement(i) == rhs.GetArrayElement(i))) return false;
        return true;
      }

      case ValueKind::kStruct: {
        if (GetStructNumBases() != rhs.GetStructNumBases()) return false;
        if (GetStructNumFields() != rhs.GetStructNumFields()) return false;
        for (unsigned i = 0; i < GetStructNumBases(); ++i)
          if (!(GetStructBase(i) == rhs.GetStructBase(i))) return false;
        for (unsigned i = 0; i < GetStructNumFields(); ++i)
          if (!(GetStructField(i) == rhs.GetStructField(i))) return false;
        return true;
      }

      case ValueKind::kUnion:
        if (GetUnionFieldIndex() != rhs.GetUnionFieldIndex()) return false;
        if (!HasUnionValue() && !rhs.HasUnionValue()) return true;
        if (!HasUnionValue() || !rhs.HasUnionValue()) return false;
        return GetUnionValue() == rhs.GetUnionValue();

      case ValueKind::kAddrLabelDiff:
        return GetAddrLabelDiffLHS() == rhs.GetAddrLabelDiffLHS() &&
               GetAddrLabelDiffRHS() == rhs.GetAddrLabelDiffRHS();

      case ValueKind::kLValue:
        return GetLValue() == rhs.GetLValue();
    }

    return false;
  }

  bool operator!=(const APValue& rhs) const { return !(*this == rhs); }

  std::string ToString() const {
    switch (GetKind()) {
      case ValueKind::kNone:
        return "<none>";
      case ValueKind::kInt:
        return GetInt().ToString(10);
      case ValueKind::kFloat: {
        return GetFloat().ToString();
      }
      case ValueKind::kComplexInt:
        return "(" + GetComplexIntReal().ToString(10) + "+" +
               GetComplexIntImag().ToString(10) + "i)";
      case ValueKind::kComplexFloat:
        return "(complex-float)";
      case ValueKind::kVector: {
        std::string s = "<";
        for (unsigned i = 0, n = GetVectorLength(); i < n; ++i) {
          if (i) s += ", ";
          s += GetVectorElt(i).ToString();
        }
        return s + ">";
      }
      case ValueKind::kArray: {
        std::string s = "{";
        for (unsigned i = 0, n = GetArraySize(); i < n; ++i) {
          if (i) s += ", ";
          s += GetArrayElement(i).ToString();
        }
        return s + "}";
      }
      case ValueKind::kStruct: {
        std::string s = "{";
        bool first = true;
        for (unsigned i = 0; i < GetStructNumBases(); ++i) {
          if (!first) s += ", ";
          first = false;
          s += GetStructBase(i).ToString();
        }
        for (unsigned i = 0; i < GetStructNumFields(); ++i) {
          if (!first) s += ", ";
          first = false;
          s += GetStructField(i).ToString();
        }
        return s + "}";
      }
      case ValueKind::kUnion:
        return "{." + std::to_string(GetUnionFieldIndex()) + "=" +
               (HasUnionValue() ? GetUnionValue().ToString() : "<none>") + "}";
      case ValueKind::kAddrLabelDiff:
        return "&&" + std::to_string(GetAddrLabelDiffLHS()) + " - &&" +
               std::to_string(GetAddrLabelDiffRHS());

      case ValueKind::kLValue:
        if (std::holds_alternative<LValueVal::NullPtr>(GetLValue().base))
          return "nullptr";

        if (std::holds_alternative<LValueVal::GlobalVar>(GetLValue().base))
          return "global@";

        if (std::holds_alternative<LValueVal::Function>(GetLValue().base))
          return "function@";

        if (std::holds_alternative<LValueVal::StringLit>(GetLValue().base))
          return "string-literal@";

        if (std::holds_alternative<LValueVal::CompoundLiteral>(
                GetLValue().base))
          return "compound-literal@";
    }

    return "<unknown>";
  }
};

}  // namespace bcc
