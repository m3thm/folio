cmake_minimum_required(VERSION 3.25)

project(folio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


# Core library: lexer, parser, AST for the folio language (.folio files).
# Everything reusable by both the CLI executable and later, tests lives here.

add_library(folio_core
    src/lexer/lexer.cpp
    src/parser/parser.cpp
)

target_include_directories(folio_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(folio_core PUBLIC cxx_std_20)

if (MSVC)
    target_compile_options(folio_core PRIVATE /W4)
else()
    target_compile_options(folio_core PRIVATE -Wall -Wextra -Wpedantic)
endif()


# CLI executable — will parse a .folio file passed as argv[1].

add_executable(folioc src/main.cpp)
target_link_libraries(folioc PRIVATE folio_core)


# add gtest to vcpkg.json's dependencies list, and write tests/lexer_tests.cpp before uncommenting this
#
# option(FOLIO_BUILD_TESTS "Build the test suite" ON)
#
# if (FOLIO_BUILD_TESTS)
#     enable_testing()
#     find_package(GTest CONFIG REQUIRED)
#
#     add_executable(folio_tests
#         tests/lexer_tests.cpp
#     )
#     target_link_libraries(folio_tests PRIVATE folio_core GTest::gtest GTest::gtest_main)
#
#     include(GoogleTest)
#     gtest_discover_tests(folio_tests)
# endif()
