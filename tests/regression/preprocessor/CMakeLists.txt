# Copy all regression-test source files to the build directory so the
# runner can discover and preprocess them with both bcc and clang.
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/macro
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/include
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/conditional
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/pragma
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/token_paste
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/stringify
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/vararg
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/spacing
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/clang_c
     DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

add_executable(bcc_pp_regression
  runner.cc
)

target_link_libraries(bcc_pp_regression
  PRIVATE
    bcc_pp
    GTest::gtest_main
)

target_compile_definitions(bcc_pp_regression
  PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_BINARY_DIR}"
)

include(GoogleTest)
gtest_discover_tests(bcc_pp_regression)
