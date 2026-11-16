cmake_minimum_required(VERSION 3.20)

project(bcc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(src)
add_subdirectory(tests)

# Built-in compiler headers (stddef.h, stdarg.h, ...). Drivers and tests link
# this interface target to learn the source-tree location at compile time.
add_library(bcc_builtin_headers INTERFACE)
target_compile_definitions(bcc_builtin_headers INTERFACE
  BCC_BUILTIN_HEADERS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/lib/Headers")

add_executable(token_dump bin/token_dump.cc)
target_link_libraries(token_dump PRIVATE bcc_lex)
target_compile_features(token_dump PUBLIC cxx_std_20)

add_executable(pp_dump bin/pp_dump.cc)
target_link_libraries(pp_dump PRIVATE bcc_pp bcc_builtin_headers)
target_compile_features(pp_dump PUBLIC cxx_std_20)

add_executable(cc_dump bin/cc_dump.cc)
target_link_libraries(cc_dump PRIVATE bcc_parse bcc_codegen bcc_builtin_headers)
target_compile_features(cc_dump PUBLIC cxx_std_20)

add_executable(diag_demo bin/diag_demo.cc)
target_link_libraries(diag_demo PRIVATE bcc_basic)
target_compile_features(diag_demo PUBLIC cxx_std_20)

add_executable(bcc_as_tool bin/bcc_as.cc)
set_target_properties(bcc_as_tool PROPERTIES OUTPUT_NAME bcc-as)
target_link_libraries(bcc_as_tool PRIVATE bcc_as)
target_compile_features(bcc_as_tool PUBLIC cxx_std_20)
