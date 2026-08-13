function(accel_aux_benchmark_slug BENCHMARK OUT_VAR)
    if("${BENCHMARK}" STREQUAL "tpch")
        set(${OUT_VAR} "tpch" PARENT_SCOPE)
    else()
        set(${OUT_VAR} "${BENCHMARK}" PARENT_SCOPE)
    endif()
endfunction()

function(accel_aux_state_dir BENCHMARK QUERY_NUM OUT_VAR)
    set(${OUT_VAR} "src/generated/${BENCHMARK}/q${QUERY_NUM}/aux" PARENT_SCOPE)
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
    set(_sources
        ${_dir}/finalize.cpp
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

function(accel_add_aux_cost_model_calibrator)
    set(oneValueArgs BENCHMARK QUERY_NUM)
    cmake_parse_arguments(CAL "" "${oneValueArgs}" "" ${ARGN})

    if(NOT CAL_QUERY_NUM)
        message(FATAL_ERROR "accel_add_aux_cost_model_calibrator requires QUERY_NUM")
    endif()
    if(NOT CAL_BENCHMARK)
        set(CAL_BENCHMARK "tpch")
    endif()

    accel_aux_benchmark_slug("${CAL_BENCHMARK}" _bench_slug)
    set(_wrapper "benchmarks/${_bench_slug}/single-threaded/calibrate_q${CAL_QUERY_NUM}_aux_cost_model.cpp")
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_wrapper}")
        return()
    endif()

    set(_target "calibrate_${_bench_slug}_q${CAL_QUERY_NUM}_aux_cost_model")
    if(TARGET ${_target})
        return()
    endif()

    add_executable(${_target}
        ${_wrapper}
        benchmarks/tpch/single-threaded/calibrate_aux_cost_model_common.cpp
        tests/test_support/cross_accel_helpers.cpp
        tests/test_support/multi_connection_stress.cpp
    )
    target_link_libraries(${_target}
        PRIVATE
            accelerator_codegen
            duckdb_sibling
    )
    target_include_directories(${_target}
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/tpch/single-threaded
    )
endfunction()

function(accel_add_tpch_aux_pipeline_targets QUERY_NUM)
    set(_q "${QUERY_NUM}")
    set(_test_src "tests/single-threaded/test_tpch_q${_q}_aux.cpp")
    set(_bench_src_per_query "benchmarks/tpch/single-threaded/benchmark_q${_q}_aux.cpp")
    set(_bench_src_sweep "benchmarks/tpch/single-threaded/benchmark_tpch_aux_sweep.cpp")
    set(_cal_src "benchmarks/tpch/single-threaded/calibrate_q${_q}_aux_cost_model.cpp")

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_test_src}")
        set(_test_tgt "test_tpch_q${_q}_aux")
        if(NOT TARGET ${_test_tgt})
            add_executable(${_test_tgt} ${_test_src})
            target_link_libraries(${_test_tgt}
                PRIVATE
                    accelerator_codegen
                    duckdb_sibling
            )
            target_include_directories(${_test_tgt}
                PRIVATE
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests
            )
        endif()
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_bench_src_per_query}")
        set(_bench_tgt "benchmark_tpch_q${_q}_aux")
        if(NOT TARGET ${_bench_tgt})
            set(_bench_sources "${_bench_src_per_query}")
            set(_baseline_cache "benchmarks/tpch/single-threaded/aux_benchmark_baseline_cache.cpp")
            set(_sweep_session "benchmarks/tpch/single-threaded/aux_benchmark_sweep_session.cpp")
            set(_arg_variants "benchmarks/tpch/single-threaded/tpch_arg_variants.cpp")
            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_baseline_cache}")
                list(APPEND _bench_sources "${_baseline_cache}")
            endif()
            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_sweep_session}")
                list(APPEND _bench_sources "${_sweep_session}")
            endif()
            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_arg_variants}")
                list(APPEND _bench_sources "${_arg_variants}")
            endif()
            add_executable(${_bench_tgt} ${_bench_sources})
            target_link_libraries(${_bench_tgt}
                PRIVATE
                    accelerator_codegen
                    duckdb_sibling
            )
            target_include_directories(${_bench_tgt}
                PRIVATE
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests
                    ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/tpch/single-threaded
            )
        endif()
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_bench_src_sweep}")
        set(_bench_tgt "benchmark_tpch_q${_q}_aux")
        if(NOT TARGET ${_bench_tgt})
            set(_sweep_session "benchmarks/tpch/single-threaded/aux_benchmark_sweep_session.cpp")
            set(_arg_variants "benchmarks/tpch/single-threaded/tpch_arg_variants.cpp")
            set(_bench_sources "${_bench_src_sweep}")
            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_sweep_session}")
                list(APPEND _bench_sources "${_sweep_session}")
            endif()
            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_arg_variants}")
                list(APPEND _bench_sources "${_arg_variants}")
            endif()
            add_executable(${_bench_tgt} ${_bench_sources})
            target_compile_definitions(${_bench_tgt}
                PRIVATE
                    ACCEL_AUX_BENCH_QUERY_NUM=${_q}
            )
            target_link_libraries(${_bench_tgt}
                PRIVATE
                    accelerator_codegen
                    duckdb_sibling
            )
            target_include_directories(${_bench_tgt}
                PRIVATE
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests
                    ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/tpch/single-threaded
            )
        endif()
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_cal_src}")
        accel_add_aux_cost_model_calibrator(QUERY_NUM ${_q})
    endif()
endfunction()

function(accel_register_tpch_aux_finalize_queries LIBRARY_TARGET TEST_ROOT)
    file(GLOB _tpch_aux_finalize CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/tpch/q*/aux/finalize.cpp")
    foreach(_finalize IN LISTS _tpch_aux_finalize)
        if(_finalize MATCHES "/tpch/q([0-9]+)/aux/finalize\\.cpp$")
            set(_q "${CMAKE_MATCH_1}")
            accel_add_aux_state_query(
                QUERY_NUM ${_q}
                LIBRARY_TARGET ${LIBRARY_TARGET}
                TEST_ROOT ${TEST_ROOT}
            )
        endif()
    endforeach()
endfunction()

function(accel_register_tpch_aux_pipeline_targets)
    file(GLOB _tpch_aux_tests CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/single-threaded/test_tpch_q*_aux.cpp")
    foreach(_test_src IN LISTS _tpch_aux_tests)
        if(_test_src MATCHES "/test_tpch_q([0-9]+)_aux\\.cpp$")
            accel_add_tpch_aux_pipeline_targets("${CMAKE_MATCH_1}")
        endif()
    endforeach()
endfunction()

function(accel_register_tpch_aux_ctests)
    file(GLOB _tpch_aux_tests CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/single-threaded/test_tpch_q*_aux.cpp")
    foreach(_test_src IN LISTS _tpch_aux_tests)
        if(_test_src MATCHES "/test_tpch_q([0-9]+)_aux\\.cpp$")
            set(_q "${CMAKE_MATCH_1}")
            set(_test_tgt "test_tpch_q${_q}_aux")
            if(TARGET ${_test_tgt})
                add_test(NAME tpch_q${_q}_aux_smoke_test COMMAND ${_test_tgt})
                set_tests_properties(tpch_q${_q}_aux_smoke_test PROPERTIES RESOURCE_LOCK tpch_sf1_db)
            endif()
        endif()
    endforeach()
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
