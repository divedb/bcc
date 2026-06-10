cmake_minimum_required(VERSION 3.20)

project(bcc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(src)
add_subdirectory(tests)

add_executable(token_dump bin/token_dump.cc)
target_link_libraries(token_dump PRIVATE bcc_lex)
target_compile_features(token_dump PUBLIC cxx_std_20)
