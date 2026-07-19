add_library(bcc_support
  support/memory_buffer.cc
)

target_include_directories(bcc_support
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(bcc_support PUBLIC cxx_std_20)

add_library(bcc_basic
  basic/diagnostic.cc
  basic/diagnostic_options.cc
  basic/file_manager.cc
  basic/line_table.cc
  basic/source_manager.cc
  basic/text_diagnostic_printer.cc
)

target_include_directories(bcc_basic
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(bcc_basic PUBLIC cxx_std_20)

target_link_libraries(bcc_basic
  PUBLIC
    bcc_support
)

add_library(bcc_lex
  lex/cursor.cc
  lex/lexer.cc
  lex/numeric_literal.cc
)

target_include_directories(bcc_lex
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(bcc_lex PUBLIC cxx_std_20)

target_link_libraries(bcc_lex
  PUBLIC
    bcc_basic
)

add_library(bcc_pp
  pp/header_search.cc
  pp/identifier_table.cc
  pp/macro_args.cc
  pp/macro_print.cc
  pp/pp_directives.cc
  pp/pp_expressions.cc
  pp/pp_lexer.cc
  pp/pp_macro_expansion.cc
  pp/preprocessor.cc
  pp/scratch_buffer.cc
)

target_include_directories(bcc_pp
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(bcc_pp PUBLIC cxx_std_20)

target_link_libraries(bcc_pp
  PUBLIC
    bcc_lex
)

add_library(bcc_ast
  ast/ast_context.cc
  ast/ast_dumper.cc
  ast/decl.cc
  ast/expr.cc
  ast/type.cc
)

target_include_directories(bcc_ast
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(bcc_ast PUBLIC cxx_std_20)

target_link_libraries(bcc_ast
  PUBLIC
    bcc_pp
)

add_library(bcc_sema
  sema/const_eval.cc
  sema/decl_spec.cc
  sema/sema.cc
  sema/sema_decl.cc
  sema/sema_expr.cc
  sema/sema_init.cc
  sema/sema_stmt.cc
  sema/sema_type.cc
)

target_include_directories(bcc_sema
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(bcc_sema PUBLIC cxx_std_20)

target_link_libraries(bcc_sema
  PUBLIC
    bcc_ast
)

add_library(bcc_parse
  parse/parse_decl.cc
  parse/parse_expr.cc
  parse/parse_init.cc
  parse/parse_stmt.cc
  parse/parser.cc
)

target_include_directories(bcc_parse
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(bcc_parse PUBLIC cxx_std_20)

target_link_libraries(bcc_parse
  PUBLIC
    bcc_sema
)

# The bcc IR: a self-contained SSA representation printed as valid LLVM IR.
# Independent of the AST libraries (mirrors the llvm/IR <-> clang/AST split).
add_library(bcc_ir
  ir/instruction.cc
  ir/ir_context.cc
  ir/ir_printer.cc
  ir/value.cc
)

target_include_directories(bcc_ir
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(bcc_ir PUBLIC cxx_std_20)

# AST -> IR lowering, modeled on clang/lib/CodeGen.
add_library(bcc_codegen
  codegen/codegen_expr.cc
  codegen/codegen_function.cc
  codegen/codegen_module.cc
  codegen/codegen_stmt.cc
  codegen/codegen_types.cc
  codegen/constant_emitter.cc
)

target_include_directories(bcc_codegen
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(bcc_codegen PUBLIC cxx_std_20)

target_link_libraries(bcc_codegen
  PUBLIC
    bcc_ast
    bcc_ir
)

# Standalone x86-64 assembler (AT&T syntax -> ELF64). Self-contained: it does
# not depend on the C front-end libraries above.
add_library(bcc_as
  as/lexer.cc
  as/register.cc
  as/mccontext.cc
  as/encoder.cc
  as/parser.cc
  as/elf_writer.cc
)

target_include_directories(bcc_as
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(bcc_as PUBLIC cxx_std_20)
