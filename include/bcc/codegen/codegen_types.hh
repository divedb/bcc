#pragma once

#include <map>

#include "bcc/ast/ast_context.hh"
#include "bcc/ast/decl.hh"
#include "bcc/ast/type.hh"
#include "bcc/codegen/codegen_value.hh"
#include "bcc/ir/ir_context.hh"
#include "bcc/ir/type.hh"

namespace bcc::codegen {

/// \brief Memoized QualType -> ir::Type conversion, plus record lowering:
///        each struct becomes an ir::StructType whose natural layout
///        reproduces the AST RecordLayout (explicit padding fields), with a
///        FieldDecl -> IR-field-index map; bit-fields get BitFieldInfo
///        instead of IR fields; unions become { [size x i8] }.
class CodeGenTypes {
 public:
  CodeGenTypes(ASTContext& ast_ctx, ir::IRContext& ir_ctx)
      : ast_(ast_ctx), ir_(ir_ctx) {}

  /// The IR type an object of type \p t occupies in memory (_Bool -> i8,
  /// arrays stay arrays, records by layout). Not for function types.
  const ir::Type* Convert(QualType t);

  /// The IR function type for (sugar for) a C function type. No-proto
  /// functions become variadic with no fixed parameters.
  const ir::FunctionType* ConvertFunctionType(QualType t);

  struct RecordInfo {
    ir::StructType* type = nullptr;
    /// IR field index of each non-bit-field FieldDecl.
    std::map<const FieldDecl*, unsigned> field_index;
    /// Storage info for each bit-field FieldDecl.
    std::map<const FieldDecl*, BitFieldInfo> bit_fields;
  };

  const RecordInfo& GetRecordInfo(const RecordDecl* record);

 private:
  ASTContext& ast_;
  ir::IRContext& ir_;
  std::map<const Type*, const ir::Type*> cache_;  ///< canonical Type* keyed
  std::map<const RecordDecl*, RecordInfo> records_;
};

}  // namespace bcc::codegen
