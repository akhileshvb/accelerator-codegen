cmake_minimum_required(VERSION 3.20)

project(accelerator_codegen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(FindSiblingDuckDB)
include(AuxState)
include(BaseQuery)

add_library(accelerator_codegen SHARED
    src/duckdb_scan_adapter.cpp
    src/query_runtime.cpp
    src/register.cpp
    src/generated/tpch/single-threaded/q1_package.cpp
    src/generated/tpch/single-threaded/q1_fused_scan.cpp
    src/generated/tpch/single-threaded/q3_package.cpp
    src/generated/tpch/single-threaded/q3_kernel.cpp
    src/generated/tpch/single-threaded/q4_package.cpp
    src/generated/tpch/single-threaded/q4_kernel.cpp
    src/generated/tpch/single-threaded/q6_package.cpp
    src/generated/tpch/single-threaded/q6_kernel.cpp
    src/generated/tpch/single-threaded/q9_package.cpp
    src/generated/tpch/single-threaded/q9_kernel.cpp
    src/generated/tpch/single-threaded/q12_package.cpp
    src/generated/tpch/single-threaded/q12_kernel.cpp
    src/generated/tpch/single-threaded/q14_package.cpp
    src/generated/tpch/single-threaded/q14_kernel.cpp
    src/generated/tpch/single-threaded/q17_package.cpp
    src/generated/tpch/single-threaded/q17_kernel.cpp
    src/generated/tpch/single-threaded/q19_package.cpp
    src/generated/tpch/single-threaded/q19_kernel.cpp
    src/generated/tpch/single-threaded/q5_package.cpp
    src/generated/tpch/single-threaded/q5_kernel.cpp
    src/generated/tpch/single-threaded/q7_package.cpp
    src/generated/tpch/single-threaded/q7_kernel.cpp
    src/generated/tpch/single-threaded/q10_package.cpp
    src/generated/tpch/single-threaded/q10_kernel.cpp
    src/generated/tpch/single-threaded/q11_package.cpp
    src/generated/tpch/single-threaded/q11_kernel.cpp
    src/generated/tpch/single-threaded/q13_package.cpp
    src/generated/tpch/single-threaded/q13_kernel.cpp
    src/generated/tpch/single-threaded/q16_package.cpp
    src/generated/tpch/single-threaded/q16_kernel.cpp
    src/generated/tpch/single-threaded/q18_package.cpp
    src/generated/tpch/single-threaded/q18_kernel.cpp
    src/generated/tpch/single-threaded/q22_package.cpp
    src/generated/tpch/single-threaded/q22_kernel.cpp
    src/generated/tpch/single-threaded/q15_package.cpp
    src/generated/tpch/single-threaded/q15_kernel.cpp
    src/generated/tpch/single-threaded/q21_package.cpp
    src/generated/tpch/single-threaded/q21_kernel.cpp
    src/generated/tpch/single-threaded/q20_package.cpp
    src/generated/tpch/single-threaded/q20_kernel.cpp
    src/generated/tpch/single-threaded/q8_package.cpp
    src/generated/tpch/single-threaded/q8_kernel.cpp
    src/generated/tpch/single-threaded/q2_package.cpp
    src/generated/tpch/single-threaded/q2_kernel.cpp
    src/aux_state/invalidation_plan.cpp
    src/aux_state/catalog_dependency.cpp
    src/aux_state/partitioned_state_store.cpp
    src/aux_state/accelerator_maintenance_state.cpp
    src/aux_state/reader_mutation_epoch.cpp
    src/aux_state/execution_load.cpp
    src/aux_state/execution_gate.cpp
    src/aux_state/accelerator_registry.cpp
    src/aux_state/txn_invalidation_buffer.cpp
    src/aux_state/maintenance/maintenance_action.cpp
    src/aux_state/maintenance/online_stats.cpp
    src/aux_state/maintenance/action_selector.cpp
    src/aux_state/maintenance/staged_publication.cpp
    src/aux_state/maintenance/maintenance_diagnostics.cpp
    src/aux_state/maintenance/interference.cpp
    src/aux_state/maintenance/partition_publish.cpp
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

accel_add_aux_state_query(
    QUERY_NUM 14
    LIBRARY_TARGET accelerator_codegen
    TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
accel_add_aux_state_benchmarks(
    QUERY_NUM 14
    LIBRARY_TARGET accelerator_codegen
    TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

add_executable(benchmark_aux_multi_connection
    benchmarks/tpch/single-threaded/benchmark_aux_multi_connection.cpp
)
target_link_libraries(benchmark_aux_multi_connection PRIVATE accelerator_codegen duckdb_sibling)
target_include_directories(benchmark_aux_multi_connection PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_aux_cross_accelerator
    benchmarks/tpch/single-threaded/benchmark_aux_cross_accelerator.cpp
    tests/test_support/cross_accel_helpers.cpp
)
target_link_libraries(benchmark_aux_cross_accelerator PRIVATE accelerator_codegen duckdb_sibling)
target_include_directories(benchmark_aux_cross_accelerator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_aux_workload_multi_connection
    benchmarks/tpch/single-threaded/benchmark_aux_workload_multi_connection.cpp
    tests/test_support/cross_accel_helpers.cpp
)
target_link_libraries(benchmark_aux_workload_multi_connection PRIVATE accelerator_codegen duckdb_sibling)
target_include_directories(benchmark_aux_workload_multi_connection PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

accel_add_aux_state_query(
    QUERY_NUM 1
    LIBRARY_TARGET accelerator_codegen
    TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
accel_add_aux_state_benchmarks(
    QUERY_NUM 1
    LIBRARY_TARGET accelerator_codegen
    TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

accel_add_aux_state_query(
    QUERY_NUM 3
    LIBRARY_TARGET accelerator_codegen
    TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
accel_add_aux_state_benchmarks(
    QUERY_NUM 3
    LIBRARY_TARGET accelerator_codegen
    TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

accel_add_aux_state_query(
    QUERY_NUM 12
    LIBRARY_TARGET accelerator_codegen
    TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
accel_add_aux_state_benchmarks(
    QUERY_NUM 12
    LIBRARY_TARGET accelerator_codegen
    TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

accel_add_aux_state_query(
    QUERY_NUM 10
    LIBRARY_TARGET accelerator_codegen
    TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
accel_add_aux_state_benchmarks(
    QUERY_NUM 10
    LIBRARY_TARGET accelerator_codegen
    TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

accel_add_aux_state_query(
    QUERY_NUM 6
    LIBRARY_TARGET accelerator_codegen
    TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
accel_add_aux_state_benchmarks(
    QUERY_NUM 6
    LIBRARY_TARGET accelerator_codegen
    TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

option(ACCEL_ENABLE_PARALLEL_TPCH "Build multithreaded TPC-H accelerator variants (accel_tpch_q*_parallel)" OFF)
if(ACCEL_ENABLE_PARALLEL_TPCH)
	target_compile_definitions(accelerator_codegen PUBLIC ACCEL_ENABLE_PARALLEL_TPCH)
	# Q3 shared __int128 revenue CAS uses libatomic (__atomic_*_16).
	target_link_libraries(accelerator_codegen PRIVATE atomic)
	target_sources(accelerator_codegen PRIVATE
		src/parallel/parallel_runtime.cpp
		src/generated/tpch/parallel/q1_parallel_aligned_agg.cpp
		src/generated/tpch/parallel/q1_parallel_kernel.cpp
		src/generated/tpch/parallel/q1_parallel_package.cpp
		src/generated/tpch/parallel/q3_parallel_kernel.cpp
		src/generated/tpch/parallel/q3_parallel_package.cpp
		src/generated/tpch/parallel/q4_parallel_kernel.cpp
		src/generated/tpch/parallel/q4_parallel_package.cpp
		src/generated/tpch/parallel/q6_parallel_kernel.cpp
		src/generated/tpch/parallel/q6_parallel_package.cpp
		src/generated/tpch/parallel/q9_parallel_kernel.cpp
		src/generated/tpch/parallel/q9_parallel_package.cpp
		src/generated/tpch/parallel/q12_parallel_kernel.cpp
		src/generated/tpch/parallel/q12_parallel_package.cpp
		src/generated/tpch/parallel/q14_parallel_kernel.cpp
		src/generated/tpch/parallel/q14_parallel_package.cpp
		src/generated/tpch/parallel/q15_parallel_kernel.cpp
		src/generated/tpch/parallel/q15_parallel_package.cpp
		src/generated/tpch/parallel/q17_parallel_kernel.cpp
		src/generated/tpch/parallel/q17_parallel_package.cpp
		src/generated/tpch/parallel/q19_parallel_kernel.cpp
		src/generated/tpch/parallel/q19_parallel_package.cpp
		src/generated/tpch/parallel/q5_parallel_kernel.cpp
		src/generated/tpch/parallel/q5_parallel_package.cpp
		src/generated/tpch/parallel/q7_parallel_kernel.cpp
		src/generated/tpch/parallel/q7_parallel_package.cpp
		src/generated/tpch/parallel/q22_parallel_kernel.cpp
		src/generated/tpch/parallel/q22_parallel_package.cpp
		src/generated/tpch/parallel/q11_parallel_kernel.cpp
		src/generated/tpch/parallel/q11_parallel_package.cpp
		src/generated/tpch/parallel/q13_parallel_kernel.cpp
		src/generated/tpch/parallel/q13_parallel_package.cpp
		src/generated/tpch/parallel/q16_parallel_kernel.cpp
		src/generated/tpch/parallel/q16_parallel_package.cpp
		src/generated/tpch/parallel/q2_parallel_kernel.cpp
		src/generated/tpch/parallel/q2_parallel_package.cpp
		src/generated/tpch/parallel/q8_parallel_kernel.cpp
		src/generated/tpch/parallel/q8_parallel_package.cpp
		src/generated/tpch/parallel/q10_parallel_kernel.cpp
		src/generated/tpch/parallel/q10_parallel_package.cpp
		src/generated/tpch/parallel/q18_parallel_kernel.cpp
		src/generated/tpch/parallel/q18_parallel_package.cpp
		src/generated/tpch/parallel/q20_parallel_kernel.cpp
		src/generated/tpch/parallel/q20_parallel_package.cpp
		src/generated/tpch/parallel/q21_parallel_kernel.cpp
		src/generated/tpch/parallel/q21_parallel_package.cpp
	)
endif()

add_executable(test_registration
    tests/single-threaded/test_registration.cpp
)

target_link_libraries(test_registration
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

add_executable(test_tpch_q6
    tests/single-threaded/test_tpch_q6.cpp
)

target_link_libraries(test_tpch_q6
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q6 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q1
    tests/single-threaded/test_tpch_q1.cpp
)

target_link_libraries(test_tpch_q1
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q3
    tests/single-threaded/test_tpch_q3.cpp
)

target_link_libraries(test_tpch_q3
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q4
    tests/single-threaded/test_tpch_q4.cpp
)

target_link_libraries(test_tpch_q4
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q4 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q9
    tests/single-threaded/test_tpch_q9.cpp
)

target_link_libraries(test_tpch_q9
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q9 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q12
    tests/single-threaded/test_tpch_q12.cpp
)

target_link_libraries(test_tpch_q12
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q12 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q14
    tests/single-threaded/test_tpch_q14.cpp
)

target_link_libraries(test_tpch_q14
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q14 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_txn_invalidation
    tests/aux_state/test_txn_invalidation.cpp
)

target_link_libraries(test_txn_invalidation
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_txn_invalidation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

foreach(_aux_substrate_test IN ITEMS test_epoch_lazy_rebuild test_commit_visibility test_reader_epoch_visibility test_optimistic_publish test_execution_load test_multi_connection_olap test_concurrent_write_read test_cross_accel_correctness test_cross_accel_isolation test_aux_multi_connection_stress)
    add_executable(${_aux_substrate_test} tests/aux_state/${_aux_substrate_test}.cpp)
    target_link_libraries(${_aux_substrate_test} PRIVATE accelerator_codegen duckdb_sibling)
    target_include_directories(${_aux_substrate_test} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
endforeach()

target_sources(test_cross_accel_correctness PRIVATE tests/test_support/cross_accel_helpers.cpp)
target_sources(test_cross_accel_isolation PRIVATE tests/test_support/cross_accel_helpers.cpp)
target_sources(test_aux_multi_connection_stress PRIVATE
    tests/test_support/cross_accel_helpers.cpp
    tests/test_support/multi_connection_stress.cpp
)

add_executable(test_tpch_q17
    tests/single-threaded/test_tpch_q17.cpp
)

target_link_libraries(test_tpch_q17
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q17 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q19
    tests/single-threaded/test_tpch_q19.cpp
)

target_link_libraries(test_tpch_q19
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q19 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q5
    tests/single-threaded/test_tpch_q5.cpp
)

target_link_libraries(test_tpch_q5
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q5 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q7
    tests/single-threaded/test_tpch_q7.cpp
)

target_link_libraries(test_tpch_q7
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q7 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q10
    tests/single-threaded/test_tpch_q10.cpp
)

target_link_libraries(test_tpch_q10
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q10 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q11
    tests/single-threaded/test_tpch_q11.cpp
)

target_link_libraries(test_tpch_q11
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q11 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q13
    tests/single-threaded/test_tpch_q13.cpp
)

target_link_libraries(test_tpch_q13
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q13 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q16
    tests/single-threaded/test_tpch_q16.cpp
)

target_link_libraries(test_tpch_q16
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q16 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q18
    tests/single-threaded/test_tpch_q18.cpp
)

target_link_libraries(test_tpch_q18
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q18 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q22
    tests/single-threaded/test_tpch_q22.cpp
)

target_link_libraries(test_tpch_q22
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q22 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q15
    tests/single-threaded/test_tpch_q15.cpp
)

target_link_libraries(test_tpch_q15
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q15 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q21
    tests/single-threaded/test_tpch_q21.cpp
)

target_link_libraries(test_tpch_q21
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q21 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q20
    tests/single-threaded/test_tpch_q20.cpp
)

target_link_libraries(test_tpch_q20
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q20 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q8
    tests/single-threaded/test_tpch_q8.cpp
)

target_link_libraries(test_tpch_q8
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q8 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q2
    tests/single-threaded/test_tpch_q2.cpp
)

target_link_libraries(test_tpch_q2
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q6
    benchmarks/tpch/single-threaded/benchmark_q6.cpp
)

target_link_libraries(benchmark_tpch_q6
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q6 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q1
    benchmarks/tpch/single-threaded/benchmark_q1.cpp
)

target_link_libraries(benchmark_tpch_q1
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q3
    benchmarks/tpch/single-threaded/benchmark_q3.cpp
)

target_link_libraries(benchmark_tpch_q3
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q4
    benchmarks/tpch/single-threaded/benchmark_q4.cpp
)

target_link_libraries(benchmark_tpch_q4
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q4 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q9
    benchmarks/tpch/single-threaded/benchmark_q9.cpp
)

target_link_libraries(benchmark_tpch_q9
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q9 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q12
    benchmarks/tpch/single-threaded/benchmark_q12.cpp
)

target_link_libraries(benchmark_tpch_q12
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q12 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q14
    benchmarks/tpch/single-threaded/benchmark_q14.cpp
)

target_link_libraries(benchmark_tpch_q14
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q14 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q17
    benchmarks/tpch/single-threaded/benchmark_q17.cpp
)

target_link_libraries(benchmark_tpch_q17
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q17 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q19
    benchmarks/tpch/single-threaded/benchmark_q19.cpp
)

target_link_libraries(benchmark_tpch_q19
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q19 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q5
    benchmarks/tpch/single-threaded/benchmark_q5.cpp
)

target_link_libraries(benchmark_tpch_q5
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q5 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q7
    benchmarks/tpch/single-threaded/benchmark_q7.cpp
)

target_link_libraries(benchmark_tpch_q7
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q7 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q10
    benchmarks/tpch/single-threaded/benchmark_q10.cpp
)

target_link_libraries(benchmark_tpch_q10
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q10 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q11
    benchmarks/tpch/single-threaded/benchmark_q11.cpp
)

target_link_libraries(benchmark_tpch_q11
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q11 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q13
    benchmarks/tpch/single-threaded/benchmark_q13.cpp
)

target_link_libraries(benchmark_tpch_q13
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q13 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q16
    benchmarks/tpch/single-threaded/benchmark_q16.cpp
)

target_link_libraries(benchmark_tpch_q16
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q16 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q18
    benchmarks/tpch/single-threaded/benchmark_q18.cpp
)

target_link_libraries(benchmark_tpch_q18
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q18 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q22
    benchmarks/tpch/single-threaded/benchmark_q22.cpp
)

target_link_libraries(benchmark_tpch_q22
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q22 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_stage_c
    benchmarks/tpch/single-threaded/benchmark_stage_c.cpp
)

target_link_libraries(benchmark_tpch_stage_c
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_stage_c PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_breakdown
    benchmarks/tpch/single-threaded/benchmark_tpch_breakdown.cpp
)

target_link_libraries(benchmark_tpch_breakdown
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_breakdown PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_validated
    benchmarks/tpch/single-threaded/benchmark_tpch_validated.cpp
)

target_link_libraries(benchmark_tpch_validated
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_validated PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

set_target_properties(accelerator_codegen test_registration test_tpch_q6 test_tpch_q1 test_tpch_q3 test_tpch_q4 test_tpch_q9 test_tpch_q12 test_tpch_q14 test_txn_invalidation test_tpch_q14_aux test_tpch_q17 test_tpch_q19 test_tpch_q5 test_tpch_q7 test_tpch_q10 test_tpch_q11 test_tpch_q13 test_tpch_q16 test_tpch_q18 test_tpch_q22 test_tpch_q15 test_tpch_q21 test_tpch_q20 test_tpch_q8 test_tpch_q2 benchmark_tpch_q6 benchmark_tpch_q1 benchmark_tpch_q3 benchmark_tpch_q4 benchmark_tpch_q9 benchmark_tpch_q12 benchmark_tpch_q14 benchmark_tpch_q14_aux benchmark_tpch_q17 benchmark_tpch_q19 benchmark_tpch_q5 benchmark_tpch_q7 benchmark_tpch_q10 benchmark_tpch_q11 benchmark_tpch_q13 benchmark_tpch_q16 benchmark_tpch_q18 benchmark_tpch_q22 benchmark_tpch_stage_c benchmark_tpch_breakdown benchmark_tpch_validated PROPERTIES
    BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
    INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
)

if(ACCEL_ENABLE_PARALLEL_TPCH)
	add_executable(test_tpch_parallel
		tests/parallel/test_tpch_parallel.cpp
	)
	target_link_libraries(test_tpch_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q20_semantics
		tests/parallel/test_tpch_q20_semantics.cpp
	)
	target_link_libraries(test_tpch_q20_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q20_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q20_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q20_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q21_semantics
		tests/parallel/test_tpch_q21_semantics.cpp
	)
	target_link_libraries(test_tpch_q21_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q21_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q21_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q21_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q22_semantics
		tests/parallel/test_tpch_q22_semantics.cpp
	)
	target_link_libraries(test_tpch_q22_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q22_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q22_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q22_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q10_semantics
		tests/parallel/test_tpch_q10_semantics.cpp
	)
	target_link_libraries(test_tpch_q10_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q10_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q10_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q10_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q17_semantics
		tests/parallel/test_tpch_q17_semantics.cpp
	)
	target_link_libraries(test_tpch_q17_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q17_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q17_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q17_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q18_semantics
		tests/parallel/test_tpch_q18_semantics.cpp
	)
	target_link_libraries(test_tpch_q18_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q18_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q18_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q18_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q3_semantics
		tests/parallel/test_tpch_q3_semantics.cpp
	)
	target_link_libraries(test_tpch_q3_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q3_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q3_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q3_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q5_semantics
		tests/parallel/test_tpch_q5_semantics.cpp
	)
	target_link_libraries(test_tpch_q5_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q5_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q5_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q5_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q7_semantics
		tests/parallel/test_tpch_q7_semantics.cpp
	)
	target_link_libraries(test_tpch_q7_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q7_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q7_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q7_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q8_semantics
		tests/parallel/test_tpch_q8_semantics.cpp
	)
	target_link_libraries(test_tpch_q8_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q8_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q8_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q8_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q11_semantics
		tests/parallel/test_tpch_q11_semantics.cpp
	)
	target_link_libraries(test_tpch_q11_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q11_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q11_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q11_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q13_semantics
		tests/parallel/test_tpch_q13_semantics.cpp
	)
	target_link_libraries(test_tpch_q13_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q13_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q13_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q13_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q1_semantics
		tests/parallel/test_tpch_q1_semantics.cpp
	)
	target_link_libraries(test_tpch_q1_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q1_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q1_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q1_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q16_semantics
		tests/parallel/test_tpch_q16_semantics.cpp
	)
	target_link_libraries(test_tpch_q16_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q16_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q16_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q16_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q2_semantics
		tests/parallel/test_tpch_q2_semantics.cpp
	)
	target_link_libraries(test_tpch_q2_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q2_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q2_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q2_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q5_parallel
		benchmarks/tpch/parallel/benchmark_q5_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q5_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q5_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q5_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q5_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q7_parallel
		benchmarks/tpch/parallel/benchmark_q7_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q7_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q7_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q7_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q7_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q8_parallel
		benchmarks/tpch/parallel/benchmark_q8_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q8_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q8_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q8_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q8_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q1_parallel
		benchmarks/tpch/parallel/benchmark_q1_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q1_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q1_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q1_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q1_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q11_parallel
		benchmarks/tpch/parallel/benchmark_q11_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q11_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q11_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q11_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q11_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q13_parallel
		benchmarks/tpch/parallel/benchmark_q13_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q13_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q13_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q13_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q13_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q16_parallel
		benchmarks/tpch/parallel/benchmark_q16_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q16_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q16_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q16_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q16_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q2_parallel
		benchmarks/tpch/parallel/benchmark_q2_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q2_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q2_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q2_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q2_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q3_parallel
		benchmarks/tpch/parallel/benchmark_q3_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q3_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q3_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q3_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q3_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q18_parallel
		benchmarks/tpch/parallel/benchmark_q18_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q18_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q18_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q18_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q18_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q10_parallel
		benchmarks/tpch/parallel/benchmark_q10_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q10_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q10_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q10_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q10_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q17_parallel
		benchmarks/tpch/parallel/benchmark_q17_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q17_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q17_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q17_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q17_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q22_parallel
		benchmarks/tpch/parallel/benchmark_q22_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q22_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q22_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q22_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q22_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_wave1_parallel
		benchmarks/tpch/parallel/benchmark_wave1_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_wave1_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_wave1_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_wave1_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_wave1_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_wave5_parallel
		benchmarks/tpch/parallel/benchmark_wave5_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_wave5_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_wave5_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_wave5_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_wave5_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_scale_proxy
		benchmarks/tpch/parallel/benchmark_scale_proxy.cpp
	)
	target_compile_definitions(benchmark_tpch_scale_proxy PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_scale_proxy PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)
endif()

if(BUILD_TESTING)
    add_test(NAME registration_smoke_test COMMAND test_registration)
    add_test(NAME tpch_q6_smoke_test COMMAND test_tpch_q6)
    add_test(NAME tpch_q1_smoke_test COMMAND test_tpch_q1)
    add_test(NAME tpch_q3_smoke_test COMMAND test_tpch_q3)
    add_test(NAME tpch_q4_smoke_test COMMAND test_tpch_q4)
    add_test(NAME tpch_q9_smoke_test COMMAND test_tpch_q9)
    add_test(NAME tpch_q12_smoke_test COMMAND test_tpch_q12)
    add_test(NAME tpch_q14_smoke_test COMMAND test_tpch_q14)
    add_test(NAME txn_invalidation_unit_test COMMAND test_txn_invalidation)
    foreach(_aux_substrate_test IN ITEMS test_epoch_lazy_rebuild test_commit_visibility test_reader_epoch_visibility test_optimistic_publish test_execution_load test_multi_connection_olap test_concurrent_write_read test_cross_accel_correctness test_cross_accel_isolation test_aux_multi_connection_stress)
        add_test(NAME ${_aux_substrate_test}_unit_test COMMAND ${_aux_substrate_test})
    endforeach()
    accel_add_aux_state_ctests(14)
    accel_add_aux_state_ctests(1)
    accel_add_aux_state_ctests(3)
    accel_add_aux_state_ctests(12)
    accel_add_aux_state_ctests(10)
    accel_add_aux_state_ctests(6)
    add_test(NAME tpch_q17_smoke_test COMMAND test_tpch_q17)
    add_test(NAME tpch_q19_smoke_test COMMAND test_tpch_q19)
    add_test(NAME tpch_q5_smoke_test COMMAND test_tpch_q5)
    add_test(NAME tpch_q7_smoke_test COMMAND test_tpch_q7)
    add_test(NAME tpch_q10_smoke_test COMMAND test_tpch_q10)
    add_test(NAME tpch_q11_smoke_test COMMAND test_tpch_q11)
    add_test(NAME tpch_q13_smoke_test COMMAND test_tpch_q13)
    add_test(NAME tpch_q16_smoke_test COMMAND test_tpch_q16)
    add_test(NAME tpch_q18_smoke_test COMMAND test_tpch_q18)
    add_test(NAME tpch_q22_smoke_test COMMAND test_tpch_q22)
    add_test(NAME tpch_q15_smoke_test COMMAND test_tpch_q15)
    add_test(NAME tpch_q21_smoke_test COMMAND test_tpch_q21)
    add_test(NAME tpch_q20_smoke_test COMMAND test_tpch_q20)
    add_test(NAME tpch_q8_smoke_test COMMAND test_tpch_q8)
    add_test(NAME tpch_q2_smoke_test COMMAND test_tpch_q2)
    set_tests_properties(tpch_q6_smoke_test tpch_q1_smoke_test tpch_q3_smoke_test tpch_q4_smoke_test tpch_q9_smoke_test tpch_q12_smoke_test tpch_q14_smoke_test tpch_q14_aux_smoke_test tpch_q17_smoke_test tpch_q19_smoke_test tpch_q5_smoke_test tpch_q7_smoke_test tpch_q10_smoke_test tpch_q11_smoke_test tpch_q13_smoke_test tpch_q16_smoke_test tpch_q18_smoke_test tpch_q22_smoke_test tpch_q15_smoke_test tpch_q21_smoke_test tpch_q20_smoke_test tpch_q8_smoke_test tpch_q2_smoke_test PROPERTIES RESOURCE_LOCK tpch_sf1_db)
	if(ACCEL_ENABLE_PARALLEL_TPCH)
		add_test(NAME tpch_parallel_smoke_test COMMAND test_tpch_parallel)
		add_test(NAME tpch_q20_semantics_test COMMAND test_tpch_q20_semantics)
		add_test(NAME tpch_q21_semantics_test COMMAND test_tpch_q21_semantics)
		add_test(NAME tpch_q22_semantics_test COMMAND test_tpch_q22_semantics)
		add_test(NAME tpch_q10_semantics_test COMMAND test_tpch_q10_semantics)
		add_test(NAME tpch_q17_semantics_test COMMAND test_tpch_q17_semantics)
		add_test(NAME tpch_q18_semantics_test COMMAND test_tpch_q18_semantics)
		add_test(NAME tpch_q3_semantics_test COMMAND test_tpch_q3_semantics)
		add_test(NAME tpch_q2_semantics_test COMMAND test_tpch_q2_semantics)
		add_test(NAME tpch_q5_semantics_test COMMAND test_tpch_q5_semantics)
		add_test(NAME tpch_q7_semantics_test COMMAND test_tpch_q7_semantics)
		add_test(NAME tpch_q8_semantics_test COMMAND test_tpch_q8_semantics)
		add_test(NAME tpch_q11_semantics_test COMMAND test_tpch_q11_semantics)
		add_test(NAME tpch_q13_semantics_test COMMAND test_tpch_q13_semantics)
		add_test(NAME tpch_q1_semantics_test COMMAND test_tpch_q1_semantics)
		add_test(NAME tpch_q16_semantics_test COMMAND test_tpch_q16_semantics)
		set_tests_properties(tpch_parallel_smoke_test tpch_q20_semantics_test tpch_q21_semantics_test tpch_q22_semantics_test tpch_q10_semantics_test tpch_q17_semantics_test tpch_q18_semantics_test tpch_q3_semantics_test tpch_q2_semantics_test tpch_q5_semantics_test tpch_q7_semantics_test tpch_q8_semantics_test tpch_q11_semantics_test tpch_q13_semantics_test tpch_q1_semantics_test tpch_q16_semantics_test PROPERTIES RESOURCE_LOCK tpch_sf1_db)
	endif()
endif()

target_sources(accelerator_codegen PRIVATE
	src/generated/sec_edgar/sec_edgar_query_runtime.cpp
)

accel_add_base_query(
	BENCHMARK sec_edgar
	QUERY_NUM 1
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
	CTEST_RESOURCE_LOCK sec_edgar_db
)

accel_add_base_query(
	BENCHMARK sec_edgar
	QUERY_NUM 2
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
	CTEST_RESOURCE_LOCK sec_edgar_db
)

accel_add_base_query(
	BENCHMARK sec_edgar
	QUERY_NUM 3
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
	CTEST_RESOURCE_LOCK sec_edgar_db
)

accel_add_base_query(
	BENCHMARK sec_edgar
	QUERY_NUM 4
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
	CTEST_RESOURCE_LOCK sec_edgar_db
)

accel_add_base_query(
	BENCHMARK sec_edgar
	QUERY_NUM 6
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
	CTEST_RESOURCE_LOCK sec_edgar_db
)

accel_add_base_query(
	BENCHMARK sec_edgar
	QUERY_NUM 24
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
	CTEST_RESOURCE_LOCK sec_edgar_db
)

accel_add_aux_state_query(
	BENCHMARK sec_edgar
	QUERY_NUM 1
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
accel_add_aux_state_benchmarks(
	BENCHMARK sec_edgar
	QUERY_NUM 1
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
if(BUILD_TESTING)
	accel_add_aux_state_ctests(BENCHMARK sec_edgar QUERY_NUM 1)
endif()

accel_add_aux_state_query(
	BENCHMARK sec_edgar
	QUERY_NUM 2
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
accel_add_aux_state_benchmarks(
	BENCHMARK sec_edgar
	QUERY_NUM 2
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
if(BUILD_TESTING)
	accel_add_aux_state_ctests(BENCHMARK sec_edgar QUERY_NUM 2)
endif()

accel_add_aux_state_query(
	BENCHMARK sec_edgar
	QUERY_NUM 3
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
accel_add_aux_state_benchmarks(
	BENCHMARK sec_edgar
	QUERY_NUM 3
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
if(BUILD_TESTING)
	accel_add_aux_state_ctests(BENCHMARK sec_edgar QUERY_NUM 3)
endif()

accel_add_aux_state_query(
	BENCHMARK sec_edgar
	QUERY_NUM 4
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
accel_add_aux_state_benchmarks(
	BENCHMARK sec_edgar
	QUERY_NUM 4
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
if(BUILD_TESTING)
	accel_add_aux_state_ctests(BENCHMARK sec_edgar QUERY_NUM 4)
endif()

accel_add_aux_state_query(
	BENCHMARK sec_edgar
	QUERY_NUM 6
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
accel_add_aux_state_benchmarks(
	BENCHMARK sec_edgar
	QUERY_NUM 6
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
if(BUILD_TESTING)
	accel_add_aux_state_ctests(BENCHMARK sec_edgar QUERY_NUM 6)
endif()

accel_add_aux_state_query(
	BENCHMARK sec_edgar
	QUERY_NUM 24
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
accel_add_aux_state_benchmarks(
	BENCHMARK sec_edgar
	QUERY_NUM 24
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
if(BUILD_TESTING)
	accel_add_aux_state_ctests(BENCHMARK sec_edgar QUERY_NUM 24)
endif()

add_executable(measure_sec_edgar_aux_memory
	benchmarks/sec_edgar/single-threaded/measure_aux_memory.cpp
)
target_link_libraries(measure_sec_edgar_aux_memory PRIVATE accelerator_codegen duckdb_sibling)
