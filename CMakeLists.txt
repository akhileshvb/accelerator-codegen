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
    src/generated/tpch/q1_package.cpp
    src/generated/tpch/q1_fused_scan.cpp
    src/generated/tpch/q3_package.cpp
    src/generated/tpch/q3_kernel.cpp
    src/generated/tpch/q4_package.cpp
    src/generated/tpch/q4_kernel.cpp
    src/generated/tpch/q6_package.cpp
    src/generated/tpch/q6_kernel.cpp
    src/generated/tpch/q9_package.cpp
    src/generated/tpch/q9_kernel.cpp
    src/generated/tpch/q12_package.cpp
    src/generated/tpch/q12_kernel.cpp
    src/generated/tpch/q14_package.cpp
    src/generated/tpch/q14_kernel.cpp
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

option(ACCEL_ENABLE_PARALLEL_TPCH "Build multithreaded TPC-H accelerator variants (accel_tpch_q*_parallel)" OFF)
if(ACCEL_ENABLE_PARALLEL_TPCH)
	target_compile_definitions(accelerator_codegen PUBLIC ACCEL_ENABLE_PARALLEL_TPCH)
	# Q3 shared __int128 revenue CAS uses libatomic (__atomic_*_16).
	target_link_libraries(accelerator_codegen PRIVATE atomic)
	target_sources(accelerator_codegen PRIVATE
		src/parallel/parallel_runtime.cpp
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
	)
endif()

add_executable(test_registration
    tests/test_registration.cpp
)

target_link_libraries(test_registration
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

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

add_executable(test_tpch_q9
    tests/test_tpch_q9.cpp
)

target_link_libraries(test_tpch_q9
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q9 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q12
    tests/test_tpch_q12.cpp
)

target_link_libraries(test_tpch_q12
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q12 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q14
    tests/test_tpch_q14.cpp
)

target_link_libraries(test_tpch_q14
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q14 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

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

add_executable(benchmark_tpch_q9
    benchmarks/tpch/benchmark_q9.cpp
)

target_link_libraries(benchmark_tpch_q9
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q9 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q12
    benchmarks/tpch/benchmark_q12.cpp
)

target_link_libraries(benchmark_tpch_q12
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q12 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q14
    benchmarks/tpch/benchmark_q14.cpp
)

target_link_libraries(benchmark_tpch_q14
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q14 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_breakdown
    benchmarks/tpch/benchmark_tpch_breakdown.cpp
)

target_link_libraries(benchmark_tpch_breakdown
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_breakdown PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_validated
    benchmarks/tpch/benchmark_tpch_validated.cpp
)

target_link_libraries(benchmark_tpch_validated
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_validated PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

set_target_properties(accelerator_codegen test_registration test_tpch_q6 test_tpch_q1 test_tpch_q3 test_tpch_q4 test_tpch_q9 test_tpch_q12 test_tpch_q14 benchmark_tpch_q6 benchmark_tpch_q1 benchmark_tpch_q3 benchmark_tpch_q4 benchmark_tpch_q9 benchmark_tpch_q12 benchmark_tpch_q14 benchmark_tpch_breakdown benchmark_tpch_validated PROPERTIES
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
    set_tests_properties(tpch_q6_smoke_test tpch_q1_smoke_test tpch_q3_smoke_test tpch_q4_smoke_test tpch_q9_smoke_test tpch_q12_smoke_test tpch_q14_smoke_test PROPERTIES RESOURCE_LOCK tpch_sf1_db)
	if(ACCEL_ENABLE_PARALLEL_TPCH)
		add_test(NAME tpch_parallel_smoke_test COMMAND test_tpch_parallel)
		set_tests_properties(tpch_parallel_smoke_test PROPERTIES RESOURCE_LOCK tpch_sf1_db)
	endif()
endif()
