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
        set(${OUT_VAR} "src/aux_state/q${QUERY_NUM}" PARENT_SCOPE)
    else()
        set(${OUT_VAR} "src/aux_state/${BENCHMARK}/q${QUERY_NUM}" PARENT_SCOPE)
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
    set(_prefix "q${QUERY_NUM}")
    accel_aux_state_dir("${BENCHMARK}" "${QUERY_NUM}" _dir)
    accel_aux_finalize_basename("${BENCHMARK}" "${QUERY_NUM}" _finalize)
    set(_sources
        ${_dir}/${_finalize}
        ${_dir}/${_prefix}_aux_dependencies.cpp
        ${_dir}/${_prefix}_incremental.cpp
        ${_dir}/${_prefix}_rebuild_policy.cpp
        ${_dir}/${_prefix}_diagnostics.cpp
        ${_dir}/${_prefix}_mutation_features.cpp
        ${_dir}/${_prefix}_workload_features.cpp
        ${_dir}/${_prefix}_cost_formulas.cpp
        ${_dir}/${_prefix}_maintenance_policy.cpp
        ${_dir}/${_prefix}_decision_csv.cpp
        ${_dir}/${_prefix}_aux_state.cpp
        ${_dir}/${_prefix}_aux_kernel.cpp
        ${_dir}/${_prefix}_aux_package.cpp
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

    if(NOT AUX_TEST_ROOT)
        return()
    endif()

    set(_q ${AUX_QUERY_NUM})
    set(_prefix "q${_q}")
    accel_aux_benchmark_slug("${AUX_BENCHMARK}" _bench_slug)
    set(_test_prefix "test_${_bench_slug}_${_prefix}")

    macro(_accel_aux_add_test target_name source_file)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}")
            add_executable(${target_name} ${source_file})
            target_link_libraries(${target_name} PRIVATE ${AUX_LIBRARY_TARGET} duckdb_sibling)
            target_include_directories(${target_name} PRIVATE ${AUX_TEST_ROOT})
        endif()
    endmacro()

    _accel_aux_add_test(${_test_prefix}_incremental tests/single-threaded/${_test_prefix}_incremental.cpp)
    _accel_aux_add_test(${_test_prefix}_aux_partition_state tests/single-threaded/${_test_prefix}_aux_partition_state.cpp)
    _accel_aux_add_test(${_test_prefix}_write_policies tests/single-threaded/${_test_prefix}_write_policies.cpp)

    foreach(suffix IN ITEMS hybrid_policy cost_model staged_publication multi_partition_delta action_failure)
        _accel_aux_add_test(${_test_prefix}_${suffix} tests/single-threaded/${_test_prefix}_${suffix}.cpp)
    endforeach()

    _accel_aux_add_test(${_test_prefix}_aux tests/single-threaded/${_test_prefix}_aux.cpp)

    if(AUX_EXTRA_TESTS)
        foreach(extra_test IN LISTS AUX_EXTRA_TESTS)
            _accel_aux_add_test(${extra_test} tests/single-threaded/${extra_test}.cpp)
        endforeach()
    endif()
endfunction()

function(accel_add_aux_state_benchmarks)
    set(options "")
    set(oneValueArgs BENCHMARK QUERY_NUM LIBRARY_TARGET TEST_ROOT)
    set(multiValueArgs EXTRA_BENCHMARKS)
    cmake_parse_arguments(AUX "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT AUX_QUERY_NUM OR NOT AUX_LIBRARY_TARGET)
        message(FATAL_ERROR "accel_add_aux_state_benchmarks requires QUERY_NUM and LIBRARY_TARGET")
    endif()
    if(NOT AUX_BENCHMARK)
        set(AUX_BENCHMARK "tpch")
    endif()

    set(_q ${AUX_QUERY_NUM})
    set(_prefix "q${_q}")
    accel_aux_benchmark_slug("${AUX_BENCHMARK}" _bench_slug)
    if("${AUX_BENCHMARK}" STREQUAL "tpch")
        set(_bench_subdir "tpch/single-threaded")
    else()
        set(_bench_subdir "${AUX_BENCHMARK}/single-threaded")
    endif()

    macro(_accel_aux_add_bench target_name source_file)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source_file}")
            add_executable(${target_name} ${source_file})
            target_link_libraries(${target_name} PRIVATE ${AUX_LIBRARY_TARGET} duckdb_sibling)
            if(AUX_TEST_ROOT)
                target_include_directories(${target_name} PRIVATE ${AUX_TEST_ROOT})
            endif()
        endif()
    endmacro()

    _accel_aux_add_bench(benchmark_${_bench_slug}_${_prefix}_aux benchmarks/${_bench_subdir}/benchmark_${_prefix}_aux.cpp)
    _accel_aux_add_bench(benchmark_${_bench_slug}_${_prefix}_hybrid_eval benchmarks/${_bench_subdir}/benchmark_${_prefix}_hybrid_eval.cpp)
    _accel_aux_add_bench(calibrate_${_bench_slug}_${_prefix}_aux_cost_model benchmarks/${_bench_subdir}/calibrate_${_prefix}_aux_cost_model.cpp)

    if(AUX_TEST_ROOT AND TARGET calibrate_${_bench_slug}_${_prefix}_aux_cost_model)
        target_include_directories(calibrate_${_bench_slug}_${_prefix}_aux_cost_model PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/${_bench_subdir})
        target_sources(calibrate_${_bench_slug}_${_prefix}_aux_cost_model PRIVATE
            ${AUX_TEST_ROOT}/test_support/multi_connection_stress.cpp
            ${AUX_TEST_ROOT}/test_support/cross_accel_helpers.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/tpch/single-threaded/calibrate_aux_cost_model_common.cpp
        )
    endif()

    if(AUX_EXTRA_BENCHMARKS)
        foreach(extra_bench IN LISTS AUX_EXTRA_BENCHMARKS)
            _accel_aux_add_bench(${extra_bench} benchmarks/${_bench_subdir}/${extra_bench}.cpp)
        endforeach()
    endif()
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
