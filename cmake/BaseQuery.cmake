# Base query scaffolding for accelerator_codegen (non-aux workloads).
#
# Usage:
#   include(BaseQuery)
#   accel_add_base_query(
#     BENCHMARK sec_edgar
#     QUERY_NUM 1
#     LIBRARY_TARGET accelerator_codegen
#     TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}
#     CTEST_RESOURCE_LOCK sec_edgar_db
#   )

function(accel_add_base_query)
    set(options "")
    set(oneValueArgs BENCHMARK QUERY_NUM LIBRARY_TARGET TEST_ROOT CTEST_RESOURCE_LOCK ENGINE)
    cmake_parse_arguments(BQ "${options}" "${oneValueArgs}" "" ${ARGN})

    if(NOT BQ_BENCHMARK OR NOT BQ_QUERY_NUM OR NOT BQ_LIBRARY_TARGET)
        message(FATAL_ERROR "accel_add_base_query requires BENCHMARK, QUERY_NUM, and LIBRARY_TARGET")
    endif()

    set(_q ${BQ_QUERY_NUM})
    set(_bench ${BQ_BENCHMARK})
    if(BQ_ENGINE)
        set(_engine "${BQ_ENGINE}")
    else()
        accel_codegen_engine_name(_engine)
    endif()
    if(_engine STREQUAL "")
        message(STATUS "accel_add_base_query: skip ${_bench} q${_q} (ACCEL_CODEGEN_ENGINE unset)")
        return()
    endif()

    set(_src_dir "${CMAKE_CURRENT_SOURCE_DIR}/accelerators/${_bench}/q${_q}/${_engine}/base/single-threaded")
    if(NOT EXISTS "${_src_dir}")
        message(STATUS "accel_add_base_query: skip ${_bench} q${_q}/${_engine} (no kernel dir)")
        return()
    endif()

    if(NOT BQ_TEST_ROOT)
        return()
    endif()

    set(_test_target "test_${_bench}_q${_q}")
    set(_bench_target "benchmark_${_bench}_q${_q}")
    set(_ctest_name "${_bench}_q${_q}_smoke_test")
    set(_test_src "accelerators/${_bench}/q${_q}/tests/${_test_target}.cpp")
    set(_bench_src "accelerators/${_bench}/q${_q}/bench/benchmark_q${_q}.cpp")

    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_test_src}")
        message(STATUS "accel_add_base_query: skip ${_test_target} (missing test source)")
        return()
    endif()

    add_executable(${_test_target} ${_test_src})
    target_link_libraries(${_test_target} PRIVATE ${BQ_LIBRARY_TARGET} duckdb_sibling)
    target_include_directories(${_test_target} PRIVATE ${BQ_TEST_ROOT})

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_bench_src}")
        add_executable(${_bench_target} ${_bench_src})
        target_link_libraries(${_bench_target} PRIVATE ${BQ_LIBRARY_TARGET} duckdb_sibling)
        target_include_directories(${_bench_target} PRIVATE ${BQ_TEST_ROOT})
    endif()

    add_test(NAME ${_ctest_name} COMMAND ${_test_target})
    set_tests_properties(${_ctest_name} PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    if(_bench STREQUAL "sec_edgar")
        set_tests_properties(${_ctest_name} PROPERTIES TIMEOUT 900)
    endif()
    if(BQ_CTEST_RESOURCE_LOCK)
        set_tests_properties(${_ctest_name} PROPERTIES RESOURCE_LOCK ${BQ_CTEST_RESOURCE_LOCK})
    endif()
endfunction()
