#include "bcc/codegen/codegen_types.hh"

#include <cassert>
#include <string>

namespace bcc::codegen {

const ir::Type* CodeGenTypes::Convert(QualType t) {
  const Type* canon = t.GetCanonical().GetTypePtr();
  auto it = cache_.find(canon);
  if (it != cache_.end()) return it->second;

  const ir::Type* result = nullptr;
  switch (canon->GetTypeClass()) {
    case TypeClass::kBuiltin: {
      switch (canon->As<BuiltinType>()->GetKind()) {
        case BuiltinTypeKind::kVoid:
          result = ir_.GetVoidType();
          break;
        case BuiltinTypeKind::kBool:
          // _Bool is i8 in memory; i1 exists only transiently (§3.6).
          result = ir_.GetInt8Type();
          break;
        case BuiltinTypeKind::kFloat:
          result = ir_.GetFloatType();
          break;
        case BuiltinTypeKind::kDouble:
        case BuiltinTypeKind::kLongDouble:  // documented deviation
          result = ir_.GetDoubleType();
          break;
        default:
          result = ir_.GetIntType(
              static_cast<unsigned>(ast_.GetTypeSize(QualType(canon)) * 8));
          break;
      }
      break;
    }
    case TypeClass::kPointer:
      result = ir_.GetPointerType();
      break;
    case TypeClass::kConstantArray: {
      const auto* at = canon->As<ConstantArrayType>();
      result = ir_.GetArrayType(Convert(at->GetElementType()), at->GetSize());
      break;
    }
    case TypeClass::kIncompleteArray: {
      // Only reachable for extern declarations / flexible array members.
      const auto* at = canon->As<IncompleteArrayType>();
      result = ir_.GetArrayType(Convert(at->GetElementType()), 0);
      break;
    }
    case TypeClass::kVariableArray: {
      // VLAs are diagnosed by codegen before this matters; keep a placeholder
      // so error recovery does not crash.
      const auto* at = canon->As<VariableArrayType>();
      result = ir_.GetArrayType(Convert(at->GetElementType()), 0);
      break;
    }
    case TypeClass::kFunctionProto:
    case TypeClass::kFunctionNoProto:
      // A function in a memory-type position only appears behind a pointer.
      result = ConvertFunctionType(QualType(canon));
      break;
    case TypeClass::kRecord:
      result = GetRecordInfo(canon->As<RecordType>()->GetDecl()).type;
      break;
    case TypeClass::kEnum:
      result = ir_.GetIntType(
          static_cast<unsigned>(ast_.GetTypeSize(QualType(canon)) * 8));
      break;
    case TypeClass::kTypedef:
      assert(false && "canonical type cannot be a typedef");
      break;
  }

  cache_[canon] = result;
  return result;
}

const ir::FunctionType* CodeGenTypes::ConvertFunctionType(QualType t) {
  const Type* canon = t.GetCanonical().GetTypePtr();
  const auto* ft = canon->As<FunctionType>();
  assert(ft && "not a function type");

  QualType ret = ft->GetReturnType();
  const ir::Type* ir_ret =
      ret->IsVoidType() ? ir_.GetVoidType() : Convert(ret);

  std::vector<const ir::Type*> params;
  bool variadic = false;
  if (const auto* proto = canon->As<FunctionProtoType>()) {
    params.reserve(proto->GetNumParams());
    for (QualType p : proto->GetParamTypes()) params.push_back(Convert(p));
    variadic = proto->IsVariadic();
  } else {
    // No-proto: lower as variadic with no fixed parameters, so calls with
    // arbitrary arguments print as valid LLVM (`i32 (...)`).
    variadic = true;
  }
  return ir_.GetFunctionType(ir_ret, std::move(params), variadic);
}

const CodeGenTypes::RecordInfo& CodeGenTypes::GetRecordInfo(
    const RecordDecl* record) {
  auto it = records_.find(record);
  if (it != records_.end()) {
    // Complete the body if the record was completed after first use
    // (forward declaration referenced through a pointer).
    if (!it->second.type->IsOpaque() || !record->IsCompleteDefinition()) {
      return it->second;
    }
  } else {
    std::string prefix = record->IsUnion() ? "union." : "struct.";
    std::string tag = record->GetIdentifier()
                          ? std::string(record->GetName())
                          : std::string("anon");
    RecordInfo info;
    info.type = ir_.CreateStructType(prefix + tag);
    it = records_.emplace(record, std::move(info)).first;
    if (!record->IsCompleteDefinition()) return it->second;
  }

  RecordInfo& info = it->second;
  const RecordLayout& layout = ast_.GetRecordLayout(record);
  const auto& fields = record->GetFields();

  if (record->IsUnion()) {
    // Unions are a bag of bytes; members are accessed through the base
    // pointer (opaque pointers make this cast-free).
    info.type->SetBody({ir_.GetArrayType(ir_.GetInt8Type(), layout.size)},
                       layout.size, layout.align);
    for (size_t i = 0; i < fields.size(); ++i) {
      const FieldDecl* fd = fields[i];
      if (!fd->IsBitField() || fd->GetBitWidth() == 0) continue;
      uint64_t unit_bits = ast_.GetTypeSize(fd->GetType()) * 8;
      info.bit_fields[fd] = BitFieldInfo{
          0, static_cast<unsigned>(unit_bits), 0, fd->GetBitWidth(),
          ast_.IsSignedIntegerType(fd->GetType())};
    }
    return info;
  }

  std::vector<const ir::Type*> ir_fields;
  uint64_t offset = 0;  // bytes covered so far
  for (size_t i = 0; i < fields.size(); ++i) {
    const FieldDecl* fd = fields[i];
    uint64_t bit_offset = layout.field_offsets_bits[i];

    if (fd->IsBitField()) {
      if (fd->GetBitWidth() == 0) continue;  // alignment-only
      // The storage unit is the sizeof(declared type)-sized, naturally
      // aligned unit containing the bits; the bits themselves are covered
      // by padding fields, not IR fields.
      uint64_t unit_bits = ast_.GetTypeSize(fd->GetType()) * 8;
      uint64_t storage_offset = bit_offset / unit_bits * (unit_bits / 8);
      info.bit_fields[fd] = BitFieldInfo{
          storage_offset, static_cast<unsigned>(unit_bits),
          static_cast<unsigned>(bit_offset - storage_offset * 8),
          fd->GetBitWidth(), ast_.IsSignedIntegerType(fd->GetType())};
      continue;
    }

    uint64_t byte_offset = bit_offset / 8;
    if (byte_offset > offset) {
      ir_fields.push_back(
          ir_.GetArrayType(ir_.GetInt8Type(), byte_offset - offset));
    }
    ir_fields.push_back(Convert(fd->GetType()));
    info.field_index[fd] = static_cast<unsigned>(ir_fields.size() - 1);
    // A flexible array member is [0 x T] and occupies no bytes.
    uint64_t field_size =
        fd->GetType()->IsCompleteType() ? ast_.GetTypeSize(fd->GetType()) : 0;
    offset = byte_offset + field_size;
  }
  if (layout.size > offset) {
    ir_fields.push_back(
        ir_.GetArrayType(ir_.GetInt8Type(), layout.size - offset));
  }

  info.type->SetBody(std::move(ir_fields), layout.size, layout.align);
  return info;
}

}  // namespace bcc::codegen
