include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(bcc_basic_test
  unittest/basic/file_manager_test.cc
)

target_link_libraries(bcc_basic_test
  PRIVATE
    bcc_basic
    GTest::gtest_main
)

add_executable(bcc_lex_test
  unittest/lex/cursor_test.cc
  unittest/lex/identifier_util_test.cc
  unittest/lex/lexer_test.cc
)

target_link_libraries(bcc_lex_test
  PRIVATE
    bcc_lex
    GTest::gtest_main
)

add_executable(bcc_pp_test
  unittest/pp/conditional_test.cc
  unittest/pp/directive_test.cc
  unittest/pp/function_macro_test.cc
  unittest/pp/identifier_table_test.cc
  unittest/pp/include_test.cc
  unittest/pp/macro_expansion_test.cc
  unittest/pp/macro_print_test.cc
  unittest/pp/pp_client_test.cc
  unittest/pp/pp_lexer_test.cc
  unittest/pp/preprocessor_test.cc
)

target_link_libraries(bcc_pp_test
  PRIVATE
    bcc_pp
    GTest::gtest_main
)

add_executable(bcc_as_test
  unittest/as/assembler_test.cc
)

target_link_libraries(bcc_as_test
  PRIVATE
    bcc_as
    GTest::gtest_main
)

add_subdirectory(regression/preprocessor)
add_subdirectory(regression/lexer)

include(GoogleTest)
gtest_discover_tests(bcc_basic_test)
gtest_discover_tests(bcc_lex_test)
gtest_discover_tests(bcc_pp_test)
gtest_discover_tests(bcc_as_test)

add_executable(bcc_frontend_test
  unittest/frontend/codegen_test.cc
  unittest/frontend/parser_test.cc
  unittest/frontend/sema_test.cc
)

target_link_libraries(bcc_frontend_test
  PRIVATE
    bcc_parse
    bcc_codegen
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(bcc_frontend_test)
