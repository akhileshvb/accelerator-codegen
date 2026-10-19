cmake_minimum_required(VERSION 3.20)

project(accelerator_codegen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ACCEL_AUX_CALIBRATION
       "Build auxiliary cost-model calibration benchmarks" OFF)
option(ACCEL_CALIBRATE_AT_BUILD
       "Run shared aux cost-model calibration during the default build (requires DBs)" OFF)

# ACCEL_SANITIZE=thread instruments our targets; the sibling DuckDB stays uninstrumented.
set(ACCEL_SANITIZE "" CACHE STRING "Sanitizer for our targets (thread)")
if(ACCEL_SANITIZE STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
elseif(ACCEL_SANITIZE)
    message(FATAL_ERROR "ACCEL_SANITIZE must be empty or 'thread'")
endif()

include(CTest)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(FindSiblingDuckDB)
include(AuxState)
include(BaseQuery)
include(GeneratedSources)
include(Backend)

accel_collect_stateful_sources(ACCEL_STATEFUL_SOURCES)

add_library(accelerator_codegen SHARED ${ACCEL_STATEFUL_SOURCES})

target_include_directories(accelerator_codegen
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators
)

target_link_libraries(accelerator_codegen
    PUBLIC
        duckdb_sibling
)

# Engines: each backends/<engine>/backend.cmake (see cmake/Backend.cmake), then its eval harnesses.
accel_include_backends()
foreach(_accel_engine IN LISTS ACCEL_BACKENDS)
    include("${CMAKE_CURRENT_SOURCE_DIR}/eval/harness/${_accel_engine}.cmake" OPTIONAL)
endforeach()

accel_apply_aux_copts(substrate/aux/state/versioned_aux_store.cpp)

# SEC-EDGAR accelerators (shared state, DuckDB aux, Postgres aux/extension) are
# always compiled for this machine's CPU, so every loop can use AVX-512/AVX2/BMI,
# not just the hand-written kernels behind __builtin_cpu_supports. Source
# properties are directory-scoped, so this also reaches accelerator_codegen_pg.
# The DuckDB base kernels already get ACCEL_BASE_COPTS. The library becomes
# CPU-specific; -DACCEL_SEC_EDGAR_COPTS= (empty) restores the portable build.
set(ACCEL_SEC_EDGAR_COPTS "-march=native" CACHE STRING "extra compile options for the SEC-EDGAR accelerator sources")
if(ACCEL_SEC_EDGAR_COPTS)
    file(GLOB_RECURSE _accel_sec_edgar_srcs CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/accelerators/sec_edgar/*.cpp")
    list(FILTER _accel_sec_edgar_srcs EXCLUDE REGEX "/duckdb/base/")
    if(_accel_sec_edgar_srcs)
        separate_arguments(_opts UNIX_COMMAND "${ACCEL_SEC_EDGAR_COPTS}")
        set_property(SOURCE ${_accel_sec_edgar_srcs} APPEND PROPERTY COMPILE_OPTIONS ${_opts})
    endif()
endif()

# Verus accelerators through the host kit (substrate/verus/kit/
# verus_kit.hpp): each accelerators/<bench>/q<N>/verus_kit/<crate>/ holds the query's host
# glue (*.cpp: the pipeline's glue.cpp, or sql2verus's gen_glue.cpp) and the
# kernel's C header; the kernel is
# ${ACCEL_VERUS_LIB_DIR}/lib<crate>_verus.a (scripts/build_verus_crate.sh).
# A glue registers itself, so nothing shared names the query.
set(ACCEL_VERUS_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build-verus"
    CACHE PATH "Directory of Verus kernel static libraries (lib<crate>_verus.a)")
# A benchmark's Verus world (scripts/gen_verus_world.py: one verified store for
# all its kits, ACCEL_AUX_STORE=verus_global) is
# accelerators/<bench>/verus_world/<world>/ (world_glue.cpp) and
# ${ACCEL_VERUS_LIB_DIR}/lib<world>_verus.a (scripts/build_verus_world.sh), an
# archive holding every kit it names: linked instead of the kits' own.
file(GLOB _verus_world_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/accelerators/*/verus_world/*")
set(_verus_world_benches)
foreach(_dir IN LISTS _verus_world_dirs)
    get_filename_component(_world "${_dir}" NAME)
    set(_lib "${ACCEL_VERUS_LIB_DIR}/lib${_world}_verus.a")
    if(IS_DIRECTORY "${_dir}" AND EXISTS "${_lib}" AND EXISTS "${_dir}/world_glue.cpp")
        string(REGEX MATCH "accelerators/([^/]+)/verus_world" _m "${_dir}")
        list(APPEND _verus_world_benches "${CMAKE_MATCH_1}")
        message(STATUS "Verus world: ${_world}")
        target_sources(accelerator_codegen PRIVATE "${_dir}/world_glue.cpp")
        set_source_files_properties("${_dir}/world_glue.cpp" PROPERTIES INCLUDE_DIRECTORIES "${_dir}")
        target_link_libraries(accelerator_codegen PRIVATE "${_lib}" pthread dl m)
    endif()
endforeach()
file(GLOB _verus_kit_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/accelerators/*/q*/verus_kit/*")
# ACCEL_VERUS_ONLY_KITS: a ;-list of kit crates (e.g. tpch_q9_vgen) whose glue
# is built; the others are left out (unregistered).  For working on one kit
# while other kits' sources are mid-change.  Empty (default): every kit.
set(ACCEL_VERUS_ONLY_KITS "" CACHE STRING "Build only these Verus kits' glue (;-list); empty = all")
foreach(_dir IN LISTS _verus_kit_dirs)
    file(GLOB _verus_kit_srcs CONFIGURE_DEPENDS "${_dir}/*.cpp")
    get_filename_component(_crate "${_dir}" NAME)
    if(ACCEL_VERUS_ONLY_KITS AND NOT _crate IN_LIST ACCEL_VERUS_ONLY_KITS)
        continue()
    endif()
    if(IS_DIRECTORY "${_dir}" AND _verus_kit_srcs)
        set(_lib "${ACCEL_VERUS_LIB_DIR}/lib${_crate}_verus.a")
        string(REGEX MATCH "accelerators/([^/]+)/q" _m "${_dir}")
        set(_in_world FALSE)
        if("${CMAKE_MATCH_1}" IN_LIST _verus_world_benches)
            set(_in_world TRUE)
        endif()
        if(EXISTS "${_lib}" OR _in_world)
            message(STATUS "Verus kit accelerator: ${_crate}")
            target_sources(accelerator_codegen PRIVATE ${_verus_kit_srcs})
            set_source_files_properties(${_verus_kit_srcs} PROPERTIES INCLUDE_DIRECTORIES "${_dir}")
            if(NOT _in_world)
                target_link_libraries(accelerator_codegen PRIVATE "${_lib}" pthread dl m)
            endif()
        endif()
    endif()
endforeach()

target_compile_features(accelerator_codegen PUBLIC cxx_std_17)

# Link finalize.cpp for every TPC-H aux module that exists on disk.
accel_register_tpch_aux_finalize_queries(
    accelerator_codegen
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Register test/bench/calibrate targets for every DuckDB TPC-H aux module present.
accel_register_tpch_aux_pipeline_targets()

add_executable(test_registration
    eval/tests/test_registration.cpp
)

target_link_libraries(test_registration
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

# Test / benchmark executable linking the library and DuckDB, with tests/ on
# the include path.  Extra arguments are the sources.
function(accel_add_duckdb_exe name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE accelerator_codegen duckdb_sibling)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)
endfunction()

# Same, for the multithreaded TPC-H variants (ACCEL_ENABLE_PARALLEL_KERNELS).
function(accel_add_parallel_tpch_exe name)
    accel_add_duckdb_exe(${name} ${ARGN})
    target_compile_definitions(${name} PRIVATE ACCEL_ENABLE_PARALLEL_KERNELS)
    set_target_properties(${name} PROPERTIES
        BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
        INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}")
endfunction()

# Per-query TPC-H smoke tests (accelerators/tpch/q<N>/tests/test_tpch_q<N>.cpp) and
# single-threaded benchmarks (accelerators/tpch/q<N>/bench/benchmark_q<N>.cpp).
set(ACCEL_TPCH_SMOKE_QS 6 1 3 4 9 12 14 17 19 5 7 10 11 13 16 18 22 15 21 20 8 2)
set(ACCEL_TPCH_BENCH_QS 6 1 3 4 9 12 14 17 19 5 7 10 11 13 16 18 22)
foreach(_q IN LISTS ACCEL_TPCH_SMOKE_QS)
    accel_add_duckdb_exe(test_tpch_q${_q} accelerators/tpch/q${_q}/tests/test_tpch_q${_q}.cpp)
endforeach()
accel_add_duckdb_exe(test_aux_runtime_units eval/tests/test_aux_runtime_units.cpp)
accel_add_duckdb_exe(test_aux_runtime eval/tests/test_aux_runtime.cpp)
accel_add_duckdb_exe(test_aux_wire eval/tests/test_aux_wire.cpp)
accel_add_duckdb_exe(test_partitioned_build eval/tests/test_partitioned_build.cpp)
accel_add_duckdb_exe(test_core_helpers eval/tests/test_core_helpers.cpp)
if(BUILD_TESTING)
    add_test(NAME partitioned_build_test COMMAND test_partitioned_build)
    add_test(NAME core_helpers_test COMMAND test_core_helpers)
    add_test(NAME aux_wire_test COMMAND test_aux_wire)
    add_test(NAME aux_runtime_units_test COMMAND test_aux_runtime_units)
    add_test(NAME aux_runtime_test COMMAND test_aux_runtime)
endif()

function(accel_add_shared_interface_test target_name test_source)
    add_executable(${target_name} ${test_source})
    target_link_libraries(${target_name} PRIVATE accelerator_codegen duckdb_sibling)
    target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)
    if(BUILD_TESTING)
        add_test(NAME ${target_name}_unit_test COMMAND ${target_name})
    endif()
endfunction()

accel_add_shared_interface_test(test_sec_edgar_q1_units accelerators/sec_edgar/q1/tests/test_sec_edgar_q1_units.cpp)
target_include_directories(test_sec_edgar_q1_units PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)
accel_add_shared_interface_test(test_sec_edgar_q1_duckdb_differential accelerators/sec_edgar/q1/tests/test_sec_edgar_q1_duckdb_differential.cpp)
add_executable(test_sec_edgar_q1_shared_differential accelerators/sec_edgar/q1/tests/test_sec_edgar_q1_shared_differential.cpp)
target_link_libraries(test_sec_edgar_q1_shared_differential PRIVATE accelerator_codegen duckdb_sibling)
add_executable(benchmark_sec_edgar_q1_micro accelerators/sec_edgar/q1/bench/benchmark_sec_edgar_q1_micro.cpp)
target_link_libraries(benchmark_sec_edgar_q1_micro PRIVATE accelerator_codegen duckdb_sibling)

# Q3: randomized differential test of the aux accelerator vs plain DuckDB SQL
# (in-memory synthetic data, production DDL; no external database needed).
add_executable(test_sec_edgar_q3_differential accelerators/sec_edgar/q3/tests/test_sec_edgar_q3_differential.cpp)
target_link_libraries(test_sec_edgar_q3_differential PRIVATE accelerator_codegen duckdb_sibling)
target_include_directories(test_sec_edgar_q3_differential PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)
if(ACCEL_ENABLE_PARALLEL_AUX)
    target_compile_definitions(test_sec_edgar_q3_differential PRIVATE ACCEL_ENABLE_PARALLEL_AUX)
endif()
if(BUILD_TESTING)
    add_test(NAME test_sec_edgar_q3_differential
             COMMAND test_sec_edgar_q3_differential 300 3
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
add_executable(bench_sec_edgar_q3_aux accelerators/sec_edgar/q3/bench/bench_sec_edgar_q3_aux.cpp)
target_link_libraries(bench_sec_edgar_q3_aux PRIVATE accelerator_codegen duckdb_sibling)
add_executable(bench_q6_maintenance accelerators/sec_edgar/q6/bench/bench_q6_maintenance.cpp)
target_link_libraries(bench_q6_maintenance PRIVATE accelerator_codegen duckdb_sibling)
target_include_directories(bench_q6_maintenance PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)
add_executable(bench_q6_structural accelerators/sec_edgar/q6/bench/bench_q6_structural.cpp)
target_link_libraries(bench_q6_structural PRIVATE accelerator_codegen duckdb_sibling)
target_include_directories(bench_q6_structural PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)
add_executable(measure_q6_aux_heap accelerators/sec_edgar/q6/bench/measure_q6_aux_heap.cpp)
target_link_libraries(measure_q6_aux_heap PRIVATE accelerator_codegen duckdb_sibling)
target_include_directories(measure_q6_aux_heap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)
accel_add_shared_interface_test(test_sec_edgar_q6_state accelerators/sec_edgar/q6/tests/test_sec_edgar_q6_state.cpp)
accel_add_shared_interface_test(test_sec_edgar_q6_differential accelerators/sec_edgar/q6/tests/test_sec_edgar_q6_differential.cpp)
if(BUILD_TESTING)
    # The differential tests read config/*.json relative to the source root.
    set_tests_properties(test_sec_edgar_q6_state_unit_test PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(test_sec_edgar_q6_differential_unit_test PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    foreach(_q6_case IN ITEMS "synthetic;11;400" "synthetic;12;400;parallel" "synthetic-large;13;250")
        list(GET _q6_case 0 _q6_mode)
        list(GET _q6_case 1 _q6_seed)
        string(REPLACE ";" "_" _q6_name "${_q6_case}")
        add_test(NAME sec_edgar_q6_differential_${_q6_name}
                 COMMAND test_sec_edgar_q6_differential ${_q6_case}
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endforeach()
endif()
accel_add_shared_interface_test(test_sec_edgar_q24_duckdb_differential accelerators/sec_edgar/q24/tests/test_sec_edgar_q24_duckdb_differential.cpp)

add_executable(benchmark_sec_edgar_q24_micro
    accelerators/sec_edgar/q24/bench/benchmark_sec_edgar_q24_micro.cpp
)
target_link_libraries(benchmark_sec_edgar_q24_micro PRIVATE accelerator_codegen duckdb_sibling)

accel_add_duckdb_exe(test_shared_incremental_differential eval/tests/test_shared_incremental_differential.cpp)
accel_add_duckdb_exe(make_tpch_test_db eval/tests/make_tpch_test_db.cpp)
accel_add_duckdb_exe(make_sec_edgar_test_db eval/tests/make_sec_edgar_test_db.cpp)
accel_add_duckdb_exe(test_tpch_q18_shared_differential accelerators/tpch/q18/tests/test_tpch_q18_shared_differential.cpp)
# Generic test / bench runner for Verus accelerators of the host kit (needs no
# particular accelerator: the Verus pipeline builds it in a tree with none).
if(ACCELERATOR_CODEGEN_BUILD_DUCKDB)
    add_executable(verus_kit_runner eval/tests/verus_kit/verus_kit_runner.cpp)
    target_link_libraries(verus_kit_runner PRIVATE accelerator_codegen duckdb_sibling)
endif()

accel_add_duckdb_exe(test_tpch_shared_differential eval/tests/test_tpch_shared_differential.cpp)
accel_add_duckdb_exe(test_shared_tpch_rf_differential eval/tests/test_shared_tpch_rf_differential.cpp)
accel_add_duckdb_exe(test_fallback_state_agree eval/tests/test_fallback_state_agree.cpp)
accel_add_duckdb_exe(time_fallback_read eval/tools/time_fallback_read.cpp)
accel_add_duckdb_exe(audit_incremental_maintenance eval/tools/audit_incremental_maintenance.cpp)
add_executable(test_tpch_arg_variants_differential
    eval/tests/test_tpch_arg_variants_differential.cpp
    eval/bench/tpch/single-threaded/tpch_arg_variants.cpp
)

target_link_libraries(test_tpch_arg_variants_differential
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_arg_variants_differential
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Generic primitives library (accelerators/prims/, see its README).
# Header-only; no DBMS needed. New primitives add their tests to test_prims.cpp.
find_package(Threads REQUIRED)
add_executable(test_prims
    accelerators/prims/tests/test_prims.cpp
)
target_include_directories(test_prims PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)
target_link_libraries(test_prims PRIVATE Threads::Threads)
if(BUILD_TESTING)
    add_test(NAME prims_unit_test COMMAND test_prims)
endif()

add_executable(test_scratch_dir eval/tests/test_scratch_dir.cpp)
target_include_directories(test_scratch_dir PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_aux_calibration
    eval/tests/test_aux_calibration.cpp
)

target_link_libraries(test_aux_calibration
    PRIVATE
        accelerator_codegen
)

target_include_directories(test_aux_calibration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)
target_compile_definitions(test_aux_calibration PRIVATE ACCEL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(test_versioned_aux_store_caps
    eval/tests/test_versioned_aux_store_caps.cpp
)
target_link_libraries(test_versioned_aux_store_caps PRIVATE accelerator_codegen)
target_include_directories(test_versioned_aux_store_caps PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(BUILD_TESTING)
    add_test(NAME versioned_aux_store_caps_test COMMAND test_versioned_aux_store_caps)
endif()

add_executable(benchmark_shared_duckdb_write_interval
    eval/bench/aux_state/benchmark_shared_duckdb_write_interval.cpp
)

target_link_libraries(benchmark_shared_duckdb_write_interval
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

add_executable(bench_sec_edgar_batch_apply
    eval/bench/aux_state/bench_sec_edgar_batch_apply.cpp
)
target_link_libraries(bench_sec_edgar_batch_apply PRIVATE accelerator_codegen duckdb_sibling)

accel_add_duckdb_exe(test_sec_edgar_q4_duckdb_differential accelerators/sec_edgar/q4/tests/test_sec_edgar_q4_duckdb_differential.cpp)
if(ACCEL_ENABLE_PARALLEL_AUX)
    target_compile_definitions(test_sec_edgar_q4_duckdb_differential PRIVATE ACCEL_ENABLE_PARALLEL_AUX)
endif()

add_executable(measure_shared_sec_edgar_aux_memory
    eval/bench/aux_state/measure_shared_sec_edgar_aux_memory.cpp
)

target_link_libraries(measure_shared_sec_edgar_aux_memory
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

# C++ vs Verus aux accelerators: read-only and concurrent reader/writer runs
# (eval/compare/run_compare.py).
add_executable(aux_compare eval/compare/aux_compare.cpp)
target_link_libraries(aux_compare PRIVATE accelerator_codegen duckdb_sibling)

if(ACCEL_AUX_CALIBRATION)
    add_executable(calibrate_shared_aux_cost_model
        eval/bench/aux_state/calibrate_shared_aux_cost_model.cpp
    )
    target_link_libraries(calibrate_shared_aux_cost_model
        PRIVATE
            accelerator_codegen
            duckdb_sibling
    )

    set(_accel_tpch_calib_db
        "${DUCKDB_ROOT}/tpch_sf1.duckdb")
    set(_accel_sec_calib_db
        "${DUCKDB_ROOT}/sec_edgar_sf3.duckdb")
    if(EXISTS "${_accel_tpch_calib_db}" AND EXISTS "${_accel_sec_calib_db}")
        if(ACCEL_CALIBRATE_AT_BUILD)
            add_custom_target(
                calibrate_shared_aux_cost_models
                ALL
                COMMAND "${CMAKE_SOURCE_DIR}/scripts/calibrate_shared_aux_cost_models.sh"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                DEPENDS calibrate_shared_aux_cost_model
                USES_TERMINAL
                COMMENT "Calibrating shared aux cost models for all 12 queries"
            )
        else()
            add_custom_target(
                calibrate_shared_aux_cost_models
                COMMAND "${CMAKE_SOURCE_DIR}/scripts/calibrate_shared_aux_cost_models.sh"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                DEPENDS calibrate_shared_aux_cost_model
                USES_TERMINAL
                COMMENT "Calibrating shared aux cost models for all 12 queries"
            )
        endif()
    else()
        message(STATUS
            "calibrate_shared_aux_cost_models skipped: full benchmark DBs not found")
    endif()
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/eval/tests/test_cross_accel_correctness.cpp")
    foreach(_aux_gate_test IN ITEMS test_cross_accel_correctness test_cross_accel_isolation test_aux_multi_connection_stress)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/eval/tests/${_aux_gate_test}.cpp")
            add_executable(${_aux_gate_test}
                eval/tests/${_aux_gate_test}.cpp
            )
            target_link_libraries(${_aux_gate_test}
                PRIVATE
                    accelerator_codegen
                    duckdb_sibling
            )
            target_include_directories(${_aux_gate_test} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)
            target_sources(${_aux_gate_test} PRIVATE
                eval/tests/test_support/cross_accel_helpers.cpp
            )
            if("${_aux_gate_test}" STREQUAL "test_aux_multi_connection_stress")
                target_sources(${_aux_gate_test} PRIVATE
                    eval/tests/test_support/multi_connection_stress.cpp
                )
            endif()
            add_test(NAME ${_aux_gate_test} COMMAND ${_aux_gate_test})
            if("${_aux_gate_test}" STREQUAL "test_cross_accel_isolation")
                # ~8 minutes alone; much longer under a parallel ctest.
                set_tests_properties(${_aux_gate_test} PROPERTIES TIMEOUT 2400)
            endif()
        endif()
    endforeach()
endif()

# Randomized differential fuzz of the TPC-H aux accelerators with engine
# capture (eval/tests/test_tpch_aux_fuzz.cpp; run by hand, needs a DB).
# Cost-model choice vs measured action costs under different writes
# (eval/tests/maintenance_choice_demo.cpp; run by hand on a TPC-H DB).
add_executable(maintenance_choice_demo
    eval/tests/maintenance_choice_demo.cpp
    eval/tests/test_support/cross_accel_helpers.cpp
)
target_link_libraries(maintenance_choice_demo PRIVATE accelerator_codegen duckdb_sibling)
target_include_directories(maintenance_choice_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)

add_executable(test_tpch_aux_fuzz
    eval/tests/test_tpch_aux_fuzz.cpp
    eval/tests/test_support/cross_accel_helpers.cpp
)
target_link_libraries(test_tpch_aux_fuzz PRIVATE accelerator_codegen duckdb_sibling)
target_include_directories(test_tpch_aux_fuzz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)

# Tie-aware ordered comparison of floating-point ORDER BY keys
# (eval/harness/float_order_ties.hpp; header-only).
add_executable(test_float_order_ties eval/tests/test_float_order_ties.cpp)
target_include_directories(test_float_order_ties PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(BUILD_TESTING)
    add_test(NAME test_float_order_ties COMMAND test_float_order_ties)
endif()

# Snapshot consistency of the aux accelerators under concurrent commits: two
# table-function calls in one read transaction vs the reference SQL
# (eval/tests/test_aux_snapshot_reads.cpp; a short Q11 run is a ctest below).
add_executable(test_aux_snapshot_reads eval/tests/test_aux_snapshot_reads.cpp)
target_link_libraries(test_aux_snapshot_reads PRIVATE accelerator_codegen duckdb_sibling)
target_include_directories(test_aux_snapshot_reads PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)

# Read-your-own-writes: a transaction that wrote a dependency table gets the
# answer of its own SQL (eval/tests/test_aux_read_own_writes.cpp).
add_executable(test_aux_read_own_writes eval/tests/test_aux_read_own_writes.cpp)
target_link_libraries(test_aux_read_own_writes PRIVATE accelerator_codegen duckdb_sibling)
target_include_directories(test_aux_read_own_writes PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)

foreach(_q IN LISTS ACCEL_TPCH_BENCH_QS)
    accel_add_duckdb_exe(benchmark_tpch_q${_q} accelerators/tpch/q${_q}/bench/benchmark_q${_q}.cpp)
endforeach()
accel_add_duckdb_exe(benchmark_tpch_stage_c eval/bench/tpch/single-threaded/benchmark_stage_c.cpp)
accel_add_duckdb_exe(benchmark_tpch_breakdown eval/bench/tpch/single-threaded/benchmark_tpch_breakdown.cpp)
accel_add_duckdb_exe(benchmark_tpch_validated eval/bench/tpch/single-threaded/benchmark_tpch_validated.cpp)
add_executable(benchmark_tpch_rf_delta_vs_rebuild
    eval/bench/tpch/single-threaded/benchmark_tpch_rf_delta_vs_rebuild.cpp
)
target_link_libraries(benchmark_tpch_rf_delta_vs_rebuild
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)
target_include_directories(benchmark_tpch_rf_delta_vs_rebuild
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators
        ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(benchmark_tpch_rf_delta_vs_rebuild PROPERTIES
    BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
    INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
)

add_executable(benchmark_tpch_q20_phased_eval
    accelerators/tpch/q20/bench/benchmark_q20_phased_eval.cpp
)
target_link_libraries(benchmark_tpch_q20_phased_eval
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)
target_include_directories(benchmark_tpch_q20_phased_eval
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators
        ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(benchmark_tpch_q20_phased_eval PROPERTIES
    BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
    INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
)

set(_accel_rpath_targets accelerator_codegen test_registration benchmark_tpch_stage_c benchmark_tpch_breakdown benchmark_tpch_validated)
foreach(_q IN LISTS ACCEL_TPCH_SMOKE_QS)
    list(APPEND _accel_rpath_targets test_tpch_q${_q})
endforeach()
foreach(_q IN LISTS ACCEL_TPCH_BENCH_QS)
    list(APPEND _accel_rpath_targets benchmark_tpch_q${_q})
endforeach()
set_target_properties(${_accel_rpath_targets} PROPERTIES
    BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
    INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
)

if(ACCEL_ENABLE_PARALLEL_KERNELS)
	# Single-threaded + parallel stateless kernels vs reference SQL, all 22 queries.
	add_executable(stateless_matrix
		eval/bench/tpch/stateless_matrix.cpp
	)
	target_link_libraries(stateless_matrix PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(stateless_matrix PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)
	set_target_properties(stateless_matrix PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	accel_add_parallel_tpch_exe(test_tpch_parallel eval/tests/parallel/test_tpch_parallel.cpp)
	foreach(_q 20 21 22 10 17 18 3 5 7 8 11 13 1 16 2)
		if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/accelerators/tpch/q${_q}/tests/test_tpch_q${_q}_semantics.cpp")
			accel_add_parallel_tpch_exe(test_tpch_q${_q}_semantics accelerators/tpch/q${_q}/tests/test_tpch_q${_q}_semantics.cpp)
		endif()
	endforeach()
	foreach(_q 5 7 8 1 11 13 16 2 3 18 10 17 22)
		if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/accelerators/tpch/q${_q}/bench/benchmark_q${_q}_parallel.cpp")
			accel_add_parallel_tpch_exe(benchmark_tpch_q${_q}_parallel accelerators/tpch/q${_q}/bench/benchmark_q${_q}_parallel.cpp)
		endif()
	endforeach()
	accel_add_parallel_tpch_exe(benchmark_tpch_wave1_parallel eval/bench/tpch/parallel/benchmark_wave1_parallel.cpp)
	accel_add_parallel_tpch_exe(benchmark_tpch_wave5_parallel eval/bench/tpch/parallel/benchmark_wave5_parallel.cpp)
	add_executable(benchmark_tpch_scale_proxy
		eval/bench/tpch/parallel/benchmark_scale_proxy.cpp
	)
	target_compile_definitions(benchmark_tpch_scale_proxy PRIVATE ACCEL_ENABLE_PARALLEL_KERNELS)
	set_target_properties(benchmark_tpch_scale_proxy PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)
endif()

if(ACCEL_ENABLE_PARALLEL_AUX)
	add_executable(test_sec_edgar_aux_parallel
		eval/tests/parallel/test_sec_edgar_aux_parallel.cpp
	)
	target_link_libraries(test_sec_edgar_aux_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_sec_edgar_aux_parallel PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators
		${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators
	)
	target_compile_definitions(test_sec_edgar_aux_parallel PRIVATE ACCEL_ENABLE_PARALLEL_AUX)
	set_target_properties(test_sec_edgar_aux_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_shared_duckdb_write_interval_parallel
		eval/bench/aux_state/benchmark_shared_duckdb_write_interval_parallel.cpp
	)
	target_link_libraries(benchmark_shared_duckdb_write_interval_parallel
		PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_shared_duckdb_write_interval_parallel PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators
	)
	target_compile_definitions(benchmark_shared_duckdb_write_interval_parallel PRIVATE ACCEL_ENABLE_PARALLEL_AUX)
	set_target_properties(benchmark_shared_duckdb_write_interval_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)
endif()

if(BUILD_TESTING)
    # No RESOURCE_LOCKs on the shared databases: tests open them read-only or
    # work on private copies (eval/tests/test_support/db_paths.hpp), so they
    # run concurrently (ctest -j, several checkouts) without lock conflicts.
    add_test(NAME registration_smoke_test COMMAND test_registration)
    foreach(_q IN LISTS ACCEL_TPCH_SMOKE_QS)
        add_test(NAME tpch_q${_q}_smoke_test COMMAND test_tpch_q${_q})
    endforeach()
    add_test(NAME shared_aux_state_unit_test COMMAND test_shared_aux_state)
    add_test(NAME aux_calibration_unit_test COMMAND test_aux_calibration)
    add_test(NAME scratch_dir_cleanup_test COMMAND test_scratch_dir)
    # Fixture: a fresh small TPC-H DB in the build dir, built from the TPC-H
    # source DB (ACCEL_TPCH_DB, default the shared SF1, read-only) with
    # workloads/tpch/test_db_subset.sql, so tests never depend on a shared
    # tpch_test.duckdb that may be stale or modified by other runs.
    set(_accel_tpch_test_db "${CMAKE_CURRENT_BINARY_DIR}/test_dbs/tpch_test.duckdb")
    set(ACCEL_TPCH_TEST_DB_PATH "${_accel_tpch_test_db}")
    add_test(NAME tpch_test_db_fixture
             COMMAND make_tpch_test_db
                     "${CMAKE_CURRENT_SOURCE_DIR}/workloads/tpch/test_db_subset.sql"
                     "${_accel_tpch_test_db}")
    set_tests_properties(tpch_test_db_fixture PROPERTIES FIXTURES_SETUP tpch_test_db)
    # The test works on private copies of its DBs. TPC-H: the fixture's DB.
    add_test(
        NAME test_shared_incremental_differential_unit_test
        COMMAND test_shared_incremental_differential "${_accel_tpch_test_db}" -)
    set_tests_properties(
        test_shared_incremental_differential_unit_test
        PROPERTIES FIXTURES_REQUIRED tpch_test_db SKIP_RETURN_CODE 77)
    # Two accelerator calls per read transaction vs the reference SQL while a
    # writer commits: the shared runtime, and the private per-query runtime
    # (no runtime registered), which must read at the transaction's snapshot.
    foreach(_mode shared private)
        add_test(
            NAME test_aux_snapshot_reads_q11_${_mode}
            COMMAND test_aux_snapshot_reads "${_accel_tpch_test_db}" 11 3 4 ${_mode})
        set_tests_properties(
            test_aux_snapshot_reads_q11_${_mode}
            PROPERTIES FIXTURES_REQUIRED tpch_test_db TIMEOUT 300)
    endforeach()
    # Q14 under the same load: its reader threads drop the last reference to the
    # database's host while a commit is maintaining through it.
    add_test(
        NAME test_aux_snapshot_reads_q14_private
        COMMAND test_aux_snapshot_reads "${_accel_tpch_test_db}" 14 3 4 private)
    # Regression: a forced partition rebuild leaves a build running when the
    # test returns or fails; teardown must not outlive the host's state, and a
    # fallback read must survive the build's snapshot on the same connection.
    add_test(
        NAME test_tpch_shared_differential_q1_forced_rebuild_partition
        COMMAND test_tpch_shared_differential "${_accel_tpch_test_db}" 1)
    set_tests_properties(
        test_tpch_shared_differential_q1_forced_rebuild_partition
        PROPERTIES FIXTURES_REQUIRED tpch_test_db TIMEOUT 300
        ENVIRONMENT "ACCEL_FORCE_MAINTENANCE_ACTION=RebuildPartition;ACCEL_AUX_READ_THREADS=8;ACCEL_AUX_BUILD_THREADS=8;ACCEL_AUX_MAINT_THREADS=8")
    # A forced RebuildAll: the read right after the commit (the rebuild may
    # still run) and the one after it settles both match DuckDB.
    foreach(_q 2 7 13 18 21)
        add_test(
            NAME test_tpch_shared_differential_q${_q}_forced_rebuild_all
            COMMAND test_tpch_shared_differential "${_accel_tpch_test_db}" ${_q})
        set_tests_properties(
            test_tpch_shared_differential_q${_q}_forced_rebuild_all
            PROPERTIES FIXTURES_REQUIRED tpch_test_db TIMEOUT 300
            ENVIRONMENT "ACCEL_FORCE_MAINTENANCE_ACTION=RebuildAll")
    endforeach()
    set_tests_properties(test_aux_snapshot_reads_q14_private
        PROPERTIES FIXTURES_REQUIRED tpch_test_db TIMEOUT 300)
    foreach(_q 1 6)
        add_test(NAME test_aux_read_own_writes_q${_q}
                 COMMAND test_aux_read_own_writes "${_accel_tpch_test_db}" ${_q})
        set_tests_properties(test_aux_read_own_writes_q${_q}
            PROPERTIES FIXTURES_REQUIRED tpch_test_db TIMEOUT 300)
    endforeach()
    # Fixture: a fresh small SEC-EDGAR DB in the build dir, built from the SEC
    # benchmark DB (ACCEL_SEC_EDGAR_DB, default the shared SF3, read-only) with
    # workloads/sec_edgar/test_db_subset.sql. Exits 77 (skip) when the source is
    # missing or empty; tests needing the DB then skip too (see below).
    set(_accel_sec_edgar_test_db "${CMAKE_CURRENT_BINARY_DIR}/test_dbs/sec_edgar_test.duckdb")
    add_test(NAME sec_edgar_test_db_fixture
             COMMAND make_sec_edgar_test_db
                     "${CMAKE_CURRENT_SOURCE_DIR}/workloads/sec_edgar/test_db_subset.sql"
                     "${_accel_sec_edgar_test_db}")
    set_tests_properties(sec_edgar_test_db_fixture PROPERTIES
        FIXTURES_SETUP sec_edgar_test_db SKIP_RETURN_CODE 77)
    add_test(
        NAME test_shared_incremental_differential_sec_edgar_test
        COMMAND test_shared_incremental_differential - "${_accel_sec_edgar_test_db}")
    set_tests_properties(
        test_shared_incremental_differential_sec_edgar_test
        PROPERTIES FIXTURES_REQUIRED sec_edgar_test_db SKIP_RETURN_CODE 77)
    accel_register_tpch_aux_ctests()
	if(ACCEL_ENABLE_PARALLEL_KERNELS)
		add_test(NAME tpch_parallel_smoke_test COMMAND test_tpch_parallel)
		foreach(_q 20 21 22 10 17 18 3 2 5 7 8 11 13 1 16)
			if(TARGET test_tpch_q${_q}_semantics)
				add_test(NAME tpch_q${_q}_semantics_test COMMAND test_tpch_q${_q}_semantics)
			endif()
		endforeach()
	endif()
	if(ACCEL_ENABLE_PARALLEL_AUX)
		add_test(NAME sec_edgar_aux_parallel_test COMMAND test_sec_edgar_aux_parallel)
	endif()
endif()

foreach(_q 1 2 3 4 6 24)
	accel_add_base_query(
		BENCHMARK sec_edgar
		QUERY_NUM ${_q}
		LIBRARY_TARGET accelerator_codegen
		TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}
	)
endforeach()

# The SEC smoke tests read the test DB through SecEdgarSmokeDbPath()
# (ACCEL_SEC_EDGAR_DB wins when set): point them at the fixture's DB.
foreach(_q 1 2 3 4 6 24)
    if(BUILD_TESTING AND TEST sec_edgar_q${_q}_smoke_test)
        set_tests_properties(sec_edgar_q${_q}_smoke_test PROPERTIES
            FIXTURES_REQUIRED sec_edgar_test_db
            ENVIRONMENT "ACCEL_SEC_EDGAR_TEST_DB=${_accel_sec_edgar_test_db}")
    endif()
endforeach()

foreach(_q 3 4 6 24)
	accel_add_aux_state_query(
		BENCHMARK sec_edgar
		QUERY_NUM ${_q}
		LIBRARY_TARGET accelerator_codegen
		TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}
	)
endforeach()

# SEC EDGAR Q2 aux: differential test (maintenance matrix, random parameters).
accel_add_shared_interface_test(test_sec_edgar_q2_differential
	accelerators/sec_edgar/q2/tests/test_sec_edgar_q2_differential.cpp)

# SEC EDGAR Q2 aux: read-only + write-rate sweep (aux vs plain DuckDB).
add_executable(benchmark_sec_edgar_q2_sweep
	accelerators/sec_edgar/q2/bench/benchmark_sec_edgar_q2_sweep.cpp)
target_link_libraries(benchmark_sec_edgar_q2_sweep PRIVATE accelerator_codegen duckdb_sibling)
add_executable(measure_sec_edgar_q2_state
	accelerators/sec_edgar/q2/bench/measure_sec_edgar_q2_state.cpp)
target_link_libraries(measure_sec_edgar_q2_state PRIVATE accelerator_codegen duckdb_sibling)
add_executable(check_sec_edgar_q2_params
	accelerators/sec_edgar/q2/bench/check_sec_edgar_q2_params.cpp)
target_link_libraries(check_sec_edgar_q2_params PRIVATE accelerator_codegen duckdb_sibling)
# SEC EDGAR Q6 aux: (fy, stmt, uom) parameter sweep against plain SQL.
add_executable(check_sec_edgar_q6_params
	accelerators/sec_edgar/q6/bench/check_sec_edgar_q6_params.cpp)
target_link_libraries(check_sec_edgar_q6_params PRIVATE accelerator_codegen duckdb_sibling)

# Q3 parameter sweep (accel_sec_edgar_q3_aux(fy, uom)) against plain SQL on a real database.
add_executable(check_sec_edgar_q3_params
	accelerators/sec_edgar/q3/bench/check_sec_edgar_q3_params.cpp)
target_link_libraries(check_sec_edgar_q3_params PRIVATE accelerator_codegen duckdb_sibling)
