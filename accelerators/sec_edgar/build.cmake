# SEC-EDGAR accelerators: per-query tests, benchmarks and tools. Included by the root.

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

if(NOT TARGET duckdb_sibling)
    return()
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


accel_add_backend_exe(duckdb test_sec_edgar_q4_duckdb_differential accelerators/sec_edgar/q4/tests/test_sec_edgar_q4_duckdb_differential.cpp)
if(ACCEL_ENABLE_PARALLEL_AUX)
    target_compile_definitions(test_sec_edgar_q4_duckdb_differential PRIVATE ACCEL_ENABLE_PARALLEL_AUX)
endif()

# Base kernels: smoke test and benchmark of every query that has them.
accel_codegen_engine_name(_engine)
file(GLOB _base_kernel_dirs CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/accelerators/sec_edgar/q*/${_engine}/base/single-threaded")
set(_base_qs)
foreach(_dir IN LISTS _base_kernel_dirs)
    string(REGEX MATCH "/q([0-9]+)/" _m "${_dir}")
    list(APPEND _base_qs ${CMAKE_MATCH_1})
    accel_add_base_query(
        BENCHMARK sec_edgar
        QUERY_NUM ${CMAKE_MATCH_1}
        LIBRARY_TARGET accelerator_codegen
        TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}
    )
endforeach()

# The smoke tests read the test DB through SecEdgarSmokeDbPath()
# (ACCEL_SEC_EDGAR_DB wins when set): point them at the fixture's DB.
foreach(_q IN LISTS _base_qs)
    if(BUILD_TESTING AND TEST sec_edgar_q${_q}_smoke_test)
        set_tests_properties(sec_edgar_q${_q}_smoke_test PROPERTIES
            FIXTURES_REQUIRED sec_edgar_test_db
            ENVIRONMENT "ACCEL_SEC_EDGAR_TEST_DB=${ACCEL_TEST_DB_DIR}/sec_edgar_test.duckdb")
    endif()
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
