cmake_minimum_required(VERSION 3.25)

project(folio LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


# Core library: lexer, parser, AST (+ its printer) for the folio language (.folio files).
# Everything reusable by both the CLI executable and later, tests lives here.

add_library(folio_core
    src/ast/ast_printer.cpp
    src/lexer/lexer.cpp
    src/parser/parser.cpp
    src/parser/expr_parser.cpp
    src/diagnostics/diagnostics_engine.cpp)

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


# Tests (GoogleTest). Configure with -DFOLIO_BUILD_TESTS=OFF to skip them.
#
# gtest is listed in vcpkg.json, so the Windows presets fetch it automatically. On
# Linux/macOS, where there is no vcpkg, install it from your package manager
# (e.g. `apt install libgtest-dev`) or the tests are skipped with a warning
# instead of failing the configure -- building folioc needs no third-party
# dependencies, and that should stay true.
#
# Run them with `ctest --test-dir out/build/debug --output-on-failure`.
# The golden-file tests read tests/golden/ and examples/ from the source tree
# (FOLIO_SOURCE_DIR below), so they work from any build directory.

option(FOLIO_BUILD_TESTS "Build the test suite" ON)

if (FOLIO_BUILD_TESTS)
    find_package(GTest CONFIG QUIET)

    if (GTest_FOUND)
        enable_testing()

        add_executable(folio_tests
            tests/parser_tests.cpp
        )
        target_link_libraries(folio_tests PRIVATE folio_core GTest::gtest GTest::gtest_main)
        target_compile_definitions(folio_tests PRIVATE FOLIO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

        if (MSVC)
            target_compile_options(folio_tests PRIVATE /W4)
        else()
            target_compile_options(folio_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()

        include(GoogleTest)
        gtest_discover_tests(folio_tests)
    else()
        message(WARNING "GTest not found: skipping the tests (folio_tests). "
                        "Install it, or pass -DFOLIO_BUILD_TESTS=OFF to silence this.")
    endif()
endif()