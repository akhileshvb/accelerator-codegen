# Aux-state query scaffolding for accelerator_codegen.
#
# Usage:
#   include(AuxState)
#   accel_add_aux_state_query(
#     BENCHMARK tpch          # default: tpch
#     QUERY_NUM 14
#     LIBRARY_TARGET accelerator_codegen
#     TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
#   )

function(accel_aux_benchmark_slug BENCHMARK OUT_VAR)
    if("${BENCHMARK}" STREQUAL "tpch")
        set(${OUT_VAR} "tpch" PARENT_SCOPE)
    else()
        set(${OUT_VAR} "${BENCHMARK}" PARENT_SCOPE)
    endif()
endfunction()

function(accel_aux_state_dir BENCHMARK QUERY_NUM OUT_VAR)
    if("${BENCHMARK}" STREQUAL "tpch")
        set(${OUT_VAR} "src/aux_state/query/tpch/q${QUERY_NUM}" PARENT_SCOPE)
    else()
        set(${OUT_VAR} "src/aux_state/query/${BENCHMARK}/q${QUERY_NUM}" PARENT_SCOPE)
    endif()
endfunction()

function(accel_aux_finalize_basename BENCHMARK QUERY_NUM OUT_VAR)
    set(_prefix "q${QUERY_NUM}")
    if("${BENCHMARK}" STREQUAL "tpch")
        set(${OUT_VAR} "tpch_${_prefix}_finalize.cpp" PARENT_SCOPE)
    else()
        set(${OUT_VAR} "${BENCHMARK}_${_prefix}_finalize.cpp" PARENT_SCOPE)
    endif()
endfunction()

function(_accel_aux_existing_sources OUT_VAR)
    set(_existing "")
    foreach(_src IN LISTS ARGN)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
            list(APPEND _existing "${_src}")
        endif()
    endforeach()
    set(${OUT_VAR} ${_existing} PARENT_SCOPE)
endfunction()

function(accel_aux_state_library_sources OUT_VAR BENCHMARK QUERY_NUM)
    accel_aux_state_dir("${BENCHMARK}" "${QUERY_NUM}" _dir)
    accel_aux_finalize_basename("${BENCHMARK}" "${QUERY_NUM}" _finalize)
    set(_sources
        ${_dir}/${_finalize}
    )
    _accel_aux_existing_sources(_existing ${_sources})
    set(${OUT_VAR} ${_existing} PARENT_SCOPE)
endfunction()

function(accel_add_aux_state_query)
    set(options "")
    set(oneValueArgs BENCHMARK QUERY_NUM LIBRARY_TARGET TEST_ROOT)
    set(multiValueArgs EXTRA_LIBRARY_SOURCES EXTRA_TESTS)
    cmake_parse_arguments(AUX "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT AUX_QUERY_NUM)
        message(FATAL_ERROR "accel_add_aux_state_query requires QUERY_NUM")
    endif()
    if(NOT AUX_LIBRARY_TARGET)
        message(FATAL_ERROR "accel_add_aux_state_query requires LIBRARY_TARGET")
    endif()
    if(NOT AUX_BENCHMARK)
        set(AUX_BENCHMARK "tpch")
    endif()

    accel_aux_state_library_sources(_aux_sources "${AUX_BENCHMARK}" ${AUX_QUERY_NUM})
    if(NOT _aux_sources)
        message(STATUS "accel_add_aux_state_query: no aux sources yet for ${AUX_BENCHMARK} q${AUX_QUERY_NUM}")
        return()
    endif()
    if(AUX_EXTRA_LIBRARY_SOURCES)
        list(APPEND _aux_sources ${AUX_EXTRA_LIBRARY_SOURCES})
    endif()
    target_sources(${AUX_LIBRARY_TARGET} PRIVATE ${_aux_sources})
endfunction()

function(accel_add_aux_state_benchmarks)
    # Legacy per-query aux benchmarks were removed with the shared aux-state
    # interface. Use benchmarks/aux_state/benchmark_shared_duckdb_write_interval.cpp
    # and the per-query benchmark_*_aux targets under benchmarks/<workload>/ instead.
endfunction()

function(accel_add_aux_state_ctests)
    set(options "")
    set(oneValueArgs BENCHMARK QUERY_NUM)
    cmake_parse_arguments(AUX "${options}" "${oneValueArgs}" "" ${ARGN})

    if(NOT AUX_QUERY_NUM)
        if(ARGC EQUAL 1 AND ARGV0 MATCHES "^[0-9]+$")
            set(AUX_QUERY_NUM ${ARGV0})
        else()
            message(FATAL_ERROR "accel_add_aux_state_ctests requires QUERY_NUM")
        endif()
    endif()
    if(NOT AUX_BENCHMARK)
        set(AUX_BENCHMARK "tpch")
    endif()

    set(_q ${AUX_QUERY_NUM})
    set(_prefix "q${_q}")
    accel_aux_benchmark_slug("${AUX_BENCHMARK}" _bench_slug)
    set(_test_prefix "test_${_bench_slug}_${_prefix}")

    if(TARGET ${_test_prefix}_incremental)
        add_test(NAME ${_bench_slug}_${_prefix}_incremental_unit_test COMMAND ${_test_prefix}_incremental)
    endif()
    if(TARGET ${_test_prefix}_aux_partition_state)
        add_test(NAME ${_bench_slug}_${_prefix}_aux_partition_state_unit_test COMMAND ${_test_prefix}_aux_partition_state)
    endif()
    if(TARGET ${_test_prefix}_write_policies)
        add_test(NAME ${_bench_slug}_${_prefix}_write_policies_unit_test COMMAND ${_test_prefix}_write_policies)
    endif()
    foreach(suffix IN ITEMS hybrid_policy cost_model staged_publication multi_partition_delta action_failure)
        if(TARGET ${_test_prefix}_${suffix})
            add_test(NAME ${_bench_slug}_${_prefix}_${suffix}_unit_test COMMAND ${_test_prefix}_${suffix})
        endif()
    endforeach()
    if(TARGET ${_test_prefix}_aux)
        add_test(NAME ${_bench_slug}_${_prefix}_aux_smoke_test COMMAND ${_test_prefix}_aux)
    endif()
endfunction()
