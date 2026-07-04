file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/tokens
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/comments
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/unicode
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/clang_c
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

add_executable(bcc_lex_regression
  runner.cc
)

include(GoogleTest)
gtest_discover_tests(bcc_lex_regression)

target_link_libraries(bcc_lex_regression
  PRIVATE
    bcc_pp
    GTest::gtest_main
)

target_compile_definitions(bcc_lex_regression
  PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)


