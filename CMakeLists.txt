cmake_minimum_required(VERSION 3.20)
project(strata VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:preprocessor)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# strata_core — the engine. No I/O beyond the page file and the WAL.
# ---------------------------------------------------------------------------
add_library(strata_core
    src/page.cpp
    src/pager.cpp
    src/wal.cpp
    src/btree.cpp
    src/db.cpp
    src/sql/lexer.cpp
    src/sql/ast.cpp
    src/sql/parser.cpp
    src/sql/catalog.cpp
    src/sql/executor.cpp
    src/sql/session.cpp
    src/sql/slt.cpp
)
target_include_directories(strata_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
add_executable(strata_crash tools/crash_harness.cpp)
target_link_libraries(strata_crash PRIVATE strata_core)

add_executable(strata_shell tools/shell.cpp)
target_link_libraries(strata_shell PRIVATE strata_core)

add_executable(strata_slt tools/slt_runner.cpp)
target_link_libraries(strata_slt PRIVATE strata_core)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
find_package(GTest CONFIG REQUIRED)
enable_testing()

add_executable(strata_tests
    tests/page_test.cpp
    tests/pager_test.cpp
    tests/wal_test.cpp
    tests/btree_test.cpp
    tests/recovery_test.cpp
    tests/mvcc_test.cpp
    tests/hermitage_test.cpp
    tests/parser_test.cpp
    tests/executor_test.cpp
    tests/slt_test.cpp
)
target_link_libraries(strata_tests PRIVATE strata_core GTest::gtest GTest::gtest_main)

# The sqllogictest corpus lives beside the source; the binary runs from build/.
target_compile_definitions(strata_tests
    PRIVATE STRATA_TESTDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata")

include(GoogleTest)
gtest_discover_tests(strata_tests)

# ---------------------------------------------------------------------------
# Benchmarks — strata against SQLite, same machine, same durability settings.
# ---------------------------------------------------------------------------
find_package(benchmark CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

add_executable(strata_bench bench/storage_bench.cpp)
target_link_libraries(strata_bench
    PRIVATE strata_core benchmark::benchmark unofficial::sqlite3::sqlite3)
