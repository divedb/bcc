add_library(bcc_support
  support/memory_buffer.cc
)

target_include_directories(bcc_support
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(bcc_support PUBLIC cxx_std_20)

add_library(bcc_basic
  basic/file_manager.cc
  basic/line_table.cc
  basic/source_manager.cc
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
