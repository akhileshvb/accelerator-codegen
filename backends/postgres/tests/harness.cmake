# PostgreSQL eval harnesses, included by accel_include_backends after backend.cmake .

# SEC-EDGAR Q4 differential: psql client (no server SDK to build, a server with
# the extension installed to run).
add_executable(test_postgres_sec_edgar_q4_differential
    accelerators/sec_edgar/q4/tests/test_postgres_sec_edgar_q4_differential.cpp
    eval/harness/param_source.cpp)
target_include_directories(test_postgres_sec_edgar_q4_differential PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)

if(NOT TARGET accel_backend_postgres)
    return()
endif()

find_package(Threads REQUIRED)

# libpq clients of a running server with the extension installed.
include(accelerators/sec_edgar/q2/tests/PostgresSecEdgarQ2Tests.cmake)
accel_add_backend_exe(postgres test_sec_edgar_q6_postgres_differential CLIENT
    accelerators/sec_edgar/q6/tests/test_sec_edgar_q6_postgres_differential.cpp
    eval/harness/param_source.cpp)
accel_add_backend_exe(postgres test_postgres_sec_edgar_q1_differential CLIENT
    accelerators/sec_edgar/q1/tests/test_postgres_sec_edgar_q1_differential.cpp
    eval/harness/param_source.cpp)
accel_add_backend_exe(postgres benchmark_postgres_sec_edgar_q1 CLIENT
    accelerators/sec_edgar/q1/bench/benchmark_postgres_sec_edgar_q1.cpp
    eval/harness/param_source.cpp)
target_link_libraries(test_postgres_sec_edgar_q1_differential PRIVATE Threads::Threads)
target_link_libraries(benchmark_postgres_sec_edgar_q1 PRIVATE Threads::Threads)

# TPC-H differential, parameter generality, and the shard workers' shared state
# (hits across connections and under writes, read-after-commit, older snapshots,
# aborts, TRUNCATE, concurrency, killed backends, worker restarts).
accel_add_backend_exe(postgres test_postgres_tpch_fuzz backends/postgres/tests/harness/test_postgres_tpch_fuzz.cpp)
accel_add_backend_exe(postgres test_postgres_tpch_params backends/postgres/tests/harness/test_postgres_tpch_params.cpp)
accel_add_backend_exe(postgres test_postgres_shared_state backends/postgres/tests/harness/test_postgres_shared_state.cpp)
target_link_libraries(test_postgres_shared_state PRIVATE Threads::Threads)
accel_add_backend_exe(postgres test_postgres_tpch_differential backends/postgres/tests/harness/test_postgres_tpch_differential.cpp)

# Aux gate harnesses (multi-connection, cross-accel).
accel_add_backend_exe(postgres test_postgres_aux_multi_connection_stress
    backends/postgres/tests/harness/test_postgres_aux_multi_connection_stress.cpp)
target_link_libraries(test_postgres_aux_multi_connection_stress PRIVATE Threads::Threads)
accel_add_backend_exe(postgres test_postgres_cross_accel backends/postgres/tests/harness/test_postgres_cross_accel.cpp)

if(BUILD_TESTING)
    add_test(NAME postgres_tpch_differential COMMAND test_postgres_tpch_differential)
    add_test(NAME test_postgres_aux_multi_connection_stress COMMAND test_postgres_aux_multi_connection_stress)
    add_test(NAME test_postgres_cross_accel COMMAND test_postgres_cross_accel)
endif()

# The workload bench and calibrate harnesses (host_bootstrap's, as every target of this file).
# Shared SQL-vs-accel TPC-H aux bench (`--query N`), with per-query aliases expected
# by run_baseline.sh (benchmark_postgres_tpch_q<N>), and the cost-model calibrator
# (wall-clock via psql \timing).
accel_add_backend_exe(postgres benchmark_postgres_tpch_aux backends/postgres/bench/benchmark_postgres_tpch_aux.cpp)
foreach(q RANGE 1 22)
    add_custom_target(benchmark_postgres_tpch_q${q}
        DEPENDS benchmark_postgres_tpch_aux
        COMMAND ${CMAKE_COMMAND} -E create_symlink
            $<TARGET_FILE:benchmark_postgres_tpch_aux>
            ${CMAKE_BINARY_DIR}/benchmark_postgres_tpch_q${q}
    )
endforeach()
accel_add_backend_exe(postgres calibrate_postgres_tpch_aux_cost_model
    backends/postgres/bench/calibrate_postgres_tpch_aux_cost_model.cpp)
