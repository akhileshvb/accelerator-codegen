# Base query scaffolding for accelerator_codegen (non-aux workloads).
#
# Usage:
#   include(BaseQuery)
#   accel_add_base_query(
#     BENCHMARK sec_edgar
#     QUERY_NUM 1
#     LIBRARY_TARGET accelerator_codegen
#     TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
#     CTEST_RESOURCE_LOCK sec_edgar_db
#   )

function(accel_add_base_query)
    set(options "")
    set(oneValueArgs BENCHMARK QUERY_NUM LIBRARY_TARGET TEST_ROOT CTEST_RESOURCE_LOCK)
    cmake_parse_arguments(BQ "${options}" "${oneValueArgs}" "" ${ARGN})

    if(NOT BQ_BENCHMARK OR NOT BQ_QUERY_NUM OR NOT BQ_LIBRARY_TARGET)
        message(FATAL_ERROR "accel_add_base_query requires BENCHMARK, QUERY_NUM, and LIBRARY_TARGET")
    endif()

    set(_q ${BQ_QUERY_NUM})
    set(_bench ${BQ_BENCHMARK})
    set(_src_dir "src/generated/${_bench}/single-threaded")
    set(_fused_src "${CMAKE_CURRENT_SOURCE_DIR}/${_src_dir}/q${_q}_fused_scan.cpp")

    target_sources(${BQ_LIBRARY_TARGET} PRIVATE
        ${_src_dir}/q${_q}_package.cpp
        ${_src_dir}/q${_q}_kernel.cpp
    )
    if(EXISTS "${_fused_src}")
        target_sources(${BQ_LIBRARY_TARGET} PRIVATE ${_src_dir}/q${_q}_fused_scan.cpp)
    endif()

    if(NOT BQ_TEST_ROOT)
        return()
    endif()

    set(_test_target "test_${_bench}_q${_q}")
    set(_bench_target "benchmark_${_bench}_q${_q}")
    set(_ctest_name "${_bench}_q${_q}_smoke_test")

    add_executable(${_test_target} tests/single-threaded/${_test_target}.cpp)
    target_link_libraries(${_test_target} PRIVATE ${BQ_LIBRARY_TARGET} duckdb_sibling)
    target_include_directories(${_test_target} PRIVATE ${BQ_TEST_ROOT})

    add_executable(${_bench_target} benchmarks/${_bench}/single-threaded/benchmark_q${_q}.cpp)
    target_link_libraries(${_bench_target} PRIVATE ${BQ_LIBRARY_TARGET} duckdb_sibling)
    target_include_directories(${_bench_target} PRIVATE ${BQ_TEST_ROOT})

    add_test(NAME ${_ctest_name} COMMAND ${_test_target})
    set_tests_properties(${_ctest_name} PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    if(_bench STREQUAL "sec_edgar")
        set_tests_properties(${_ctest_name} PROPERTIES TIMEOUT 900)
    endif()
    if(BQ_CTEST_RESOURCE_LOCK)
        set_tests_properties(${_ctest_name} PROPERTIES RESOURCE_LOCK ${BQ_CTEST_RESOURCE_LOCK})
    endif()
endfunction()
