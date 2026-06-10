add_library(bcc_lex
  lex/cursor.cc
  lex/lexer.cc
)

target_include_directories(bcc_lex
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(bcc_lex PUBLIC cxx_std_20)
