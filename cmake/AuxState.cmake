# Aux-state query scaffolding for accelerator_codegen.
#
# Usage:
#   include(AuxState)
#   accel_add_aux_state_query(
#     QUERY_NUM 14
#     LIBRARY_TARGET accelerator_codegen
#     TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
#   )

function(accel_aux_state_library_sources OUT_VAR QUERY_NUM)
    set(_prefix "q${QUERY_NUM}")
    set(_dir "src/aux_state/${_prefix}")
    set(_sources
        ${_dir}/tpch_${_prefix}_finalize.cpp
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
    set(${OUT_VAR} ${_sources} PARENT_SCOPE)
endfunction()

function(accel_add_aux_state_query)
    set(options "")
    set(oneValueArgs QUERY_NUM LIBRARY_TARGET TEST_ROOT)
    set(multiValueArgs EXTRA_LIBRARY_SOURCES EXTRA_TESTS)
    cmake_parse_arguments(AUX "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT AUX_QUERY_NUM)
        message(FATAL_ERROR "accel_add_aux_state_query requires QUERY_NUM")
    endif()
    if(NOT AUX_LIBRARY_TARGET)
        message(FATAL_ERROR "accel_add_aux_state_query requires LIBRARY_TARGET")
    endif()

    accel_aux_state_library_sources(_aux_sources ${AUX_QUERY_NUM})
    if(AUX_EXTRA_LIBRARY_SOURCES)
        list(APPEND _aux_sources ${AUX_EXTRA_LIBRARY_SOURCES})
    endif()
    target_sources(${AUX_LIBRARY_TARGET} PRIVATE ${_aux_sources})

    if(NOT AUX_TEST_ROOT)
        return()
    endif()

    set(_q ${AUX_QUERY_NUM})
    set(_prefix "q${_q}")

    macro(_accel_aux_add_test target_name source_file)
        add_executable(${target_name} ${source_file})
        target_link_libraries(${target_name} PRIVATE ${AUX_LIBRARY_TARGET} duckdb_sibling)
        target_include_directories(${target_name} PRIVATE ${AUX_TEST_ROOT})
    endmacro()

    _accel_aux_add_test(test_${_prefix}_incremental tests/single-threaded/test_${_prefix}_incremental.cpp)
    _accel_aux_add_test(test_${_prefix}_aux_partition_state tests/single-threaded/test_${_prefix}_aux_partition_state.cpp)
    _accel_aux_add_test(test_${_prefix}_write_policies tests/single-threaded/test_${_prefix}_write_policies.cpp)

    foreach(suffix IN ITEMS hybrid_policy cost_model staged_publication multi_partition_delta action_failure)
        _accel_aux_add_test(test_${_prefix}_${suffix} tests/single-threaded/test_${_prefix}_${suffix}.cpp)
    endforeach()

    _accel_aux_add_test(test_tpch_${_prefix}_aux tests/single-threaded/test_tpch_${_prefix}_aux.cpp)

    if(AUX_EXTRA_TESTS)
        foreach(extra_test IN LISTS AUX_EXTRA_TESTS)
            _accel_aux_add_test(${extra_test} tests/single-threaded/${extra_test}.cpp)
        endforeach()
    endif()
endfunction()

function(accel_add_aux_state_benchmarks)
    set(options "")
    set(oneValueArgs QUERY_NUM LIBRARY_TARGET TEST_ROOT)
    set(multiValueArgs EXTRA_BENCHMARKS)
    cmake_parse_arguments(AUX "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT AUX_QUERY_NUM OR NOT AUX_LIBRARY_TARGET)
        message(FATAL_ERROR "accel_add_aux_state_benchmarks requires QUERY_NUM and LIBRARY_TARGET")
    endif()

    set(_q ${AUX_QUERY_NUM})
    set(_prefix "q${_q}")

    macro(_accel_aux_add_bench target_name source_file)
        add_executable(${target_name} ${source_file})
        target_link_libraries(${target_name} PRIVATE ${AUX_LIBRARY_TARGET} duckdb_sibling)
        if(AUX_TEST_ROOT)
            target_include_directories(${target_name} PRIVATE ${AUX_TEST_ROOT})
        endif()
    endmacro()

    _accel_aux_add_bench(benchmark_tpch_${_prefix}_aux benchmarks/tpch/single-threaded/benchmark_${_prefix}_aux.cpp)
    _accel_aux_add_bench(benchmark_${_prefix}_hybrid_eval benchmarks/tpch/single-threaded/benchmark_${_prefix}_hybrid_eval.cpp)
    _accel_aux_add_bench(calibrate_${_prefix}_aux_cost_model benchmarks/tpch/single-threaded/calibrate_${_prefix}_aux_cost_model.cpp)

    if(AUX_TEST_ROOT)
        target_include_directories(calibrate_${_prefix}_aux_cost_model PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/tpch/single-threaded)
        target_sources(calibrate_${_prefix}_aux_cost_model PRIVATE
            ${AUX_TEST_ROOT}/test_support/multi_connection_stress.cpp
            ${AUX_TEST_ROOT}/test_support/cross_accel_helpers.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/tpch/single-threaded/calibrate_aux_cost_model_common.cpp
        )
    endif()

    if(AUX_EXTRA_BENCHMARKS)
        foreach(extra_bench IN LISTS AUX_EXTRA_BENCHMARKS)
            _accel_aux_add_bench(${extra_bench} benchmarks/tpch/single-threaded/${extra_bench}.cpp)
        endforeach()
    endif()
endfunction()

function(accel_add_aux_state_ctests QUERY_NUM)
    set(_q ${QUERY_NUM})
    set(_prefix "q${_q}")
    add_test(NAME ${_prefix}_incremental_unit_test COMMAND test_${_prefix}_incremental)
    add_test(NAME ${_prefix}_aux_partition_state_unit_test COMMAND test_${_prefix}_aux_partition_state)
    add_test(NAME ${_prefix}_write_policies_unit_test COMMAND test_${_prefix}_write_policies)
    foreach(suffix IN ITEMS hybrid_policy cost_model staged_publication multi_partition_delta action_failure)
        add_test(NAME ${_prefix}_${suffix}_unit_test COMMAND test_${_prefix}_${suffix})
    endforeach()
    add_test(NAME tpch_${_prefix}_aux_smoke_test COMMAND test_tpch_${_prefix}_aux)
endfunction()
