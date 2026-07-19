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
    ${LLVM_INCLUDE_DIRS}
)

target_compile_features(bcc_lex PUBLIC cxx_std_20)

target_link_libraries(bcc_lex
  PUBLIC
    bcc_basic
    LLVM
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
