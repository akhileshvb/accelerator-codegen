cmake_minimum_required(VERSION 3.20)

project(accelerator_codegen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(FindSiblingDuckDB)

add_library(accelerator_codegen SHARED
    src/duckdb_scan_adapter.cpp
    src/query_runtime.cpp
    src/register.cpp
    src/functions/accelerator_hello.cpp
    src/functions/count_lineitem.cpp
    src/generated/tpch/q1_package.cpp
    src/generated/tpch/q1_kernel.cpp
    src/generated/tpch/q1_fused_scan.cpp
    src/generated/tpch/q1_aux_state.cpp
    src/generated/tpch/q3_package.cpp
    src/generated/tpch/q3_kernel.cpp
    src/generated/tpch/q4_package.cpp
    src/generated/tpch/q4_kernel.cpp
    src/generated/tpch/q6_package.cpp
    src/generated/tpch/q6_kernel.cpp
)

target_include_directories(accelerator_codegen
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(accelerator_codegen
    PUBLIC
        duckdb_sibling
)

target_compile_features(accelerator_codegen PUBLIC cxx_std_17)

add_executable(test_registration
    tests/test_registration.cpp
)

target_link_libraries(test_registration
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

add_executable(test_count_lineitem
    tests/test_count_lineitem.cpp
    tests/test_support/native_scan_test_utils.cpp
)

target_link_libraries(test_count_lineitem
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_count_lineitem PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q6
    tests/test_tpch_q6.cpp
)

target_link_libraries(test_tpch_q6
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q6 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q1
    tests/test_tpch_q1.cpp
)

target_link_libraries(test_tpch_q1
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q3
    tests/test_tpch_q3.cpp
)

target_link_libraries(test_tpch_q3
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q4
    tests/test_tpch_q4.cpp
)

target_link_libraries(test_tpch_q4
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q4 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q6
    benchmarks/tpch/benchmark_q6.cpp
)

target_link_libraries(benchmark_tpch_q6
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q6 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q1
    benchmarks/tpch/benchmark_q1.cpp
)

target_link_libraries(benchmark_tpch_q1
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_q1_aux
    benchmarks/tpch/benchmark_q1_aux.cpp
)

target_link_libraries(benchmark_q1_aux
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_q1_aux PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q3
    benchmarks/tpch/benchmark_q3.cpp
)

target_link_libraries(benchmark_tpch_q3
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q4
    benchmarks/tpch/benchmark_q4.cpp
)

target_link_libraries(benchmark_tpch_q4
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q4 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_breakdown
    benchmarks/tpch/benchmark_tpch_breakdown.cpp
)

target_link_libraries(benchmark_tpch_breakdown
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_breakdown PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

set_target_properties(accelerator_codegen test_registration test_count_lineitem test_tpch_q6 test_tpch_q1 test_tpch_q3 test_tpch_q4 benchmark_tpch_q6 benchmark_tpch_q1 benchmark_tpch_q3 benchmark_tpch_q4 benchmark_tpch_breakdown PROPERTIES
    BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
    INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
)

if(BUILD_TESTING)
    add_test(NAME registration_smoke_test COMMAND test_registration)
    add_test(NAME count_lineitem_smoke_test COMMAND test_count_lineitem)
    add_test(NAME tpch_q6_smoke_test COMMAND test_tpch_q6)
    add_test(NAME tpch_q1_smoke_test COMMAND test_tpch_q1)
    add_test(NAME tpch_q3_smoke_test COMMAND test_tpch_q3)
    add_test(NAME tpch_q4_smoke_test COMMAND test_tpch_q4)
    set_tests_properties(count_lineitem_smoke_test tpch_q6_smoke_test tpch_q1_smoke_test tpch_q3_smoke_test tpch_q4_smoke_test PROPERTIES RESOURCE_LOCK tpch_sf1_db)
endif()
