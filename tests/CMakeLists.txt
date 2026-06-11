include(FetchContent)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(bcc_lex_test
  lex/cursor_test.cc
  lex/identifier_util_test.cc
  lex/lexer_test.cc
)

target_link_libraries(bcc_lex_test
  PRIVATE
    bcc_lex
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(bcc_lex_test)
