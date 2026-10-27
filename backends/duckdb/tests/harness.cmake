# DuckDB engine harnesses and benchmarks, formerly in the root CMakeLists.txt.
# Included by accel_include_backends after backends/duckdb/backend.cmake.
# Left for the engine_backend pipeline to absorb into backend.cmake: they drive
# DuckdbRuntime and link libaccelerator_codegen against the sibling DuckDB.

target_sources(accelerator_codegen PRIVATE
    eval/harness/differential.cpp
    eval/harness/cost_calibration.cpp
    eval/harness/tpch_rf.cpp
)

# Slot gates (backend-generic scenarios) against the DuckDB host.
accel_add_backend_exe(duckdb test_slot_gates_duckdb eval/tests/test_slot_gates_duckdb.cpp eval/harness/slot_gates.cpp)
if(BUILD_TESTING)
    # ddl_events (a replaced table must not be answered from state built for the old one)
    # is left out until the DuckDB host detects that; `test_slot_gates_duckdb ddl_events` runs it.
    foreach(scenario epoch_stress hits_under_writes read_after_commit txn_edges own_writes)
        add_test(NAME slot_gates_duckdb_${scenario} COMMAND test_slot_gates_duckdb ${scenario} 4)
    endforeach()
endif()

# The module unit test of every TPC-H query with a DuckDB aux module: one
# shared source (eval/tests/test_tpch_module.cpp) built per query as
# test_tpch_q<N>_aux, run on the small test database (fixture tpch_test_db).
function(accel_register_tpch_aux_ctests)
    file(GLOB _tpch_aux_modules CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/accelerators/tpch/q*/duckdb/aux/register.cpp")
    foreach(_register IN LISTS _tpch_aux_modules)
        if(NOT _register MATCHES "/tpch/q([0-9]+)/duckdb/aux/register\\.cpp$")
            continue()
        endif()
        set(_q "${CMAKE_MATCH_1}")
        set(_test_tgt "test_tpch_q${_q}_aux")
        add_executable(${_test_tgt}
            eval/tests/test_tpch_module.cpp
            eval/bench/tpch/single-threaded/tpch_arg_variants.cpp)
        target_compile_definitions(${_test_tgt} PRIVATE ACCEL_TPCH_QUERY=${_q})
        target_link_libraries(${_test_tgt} PRIVATE accelerator_codegen duckdb_sibling)
        target_include_directories(${_test_tgt}
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)
        add_test(NAME tpch_q${_q}_aux_smoke_test
                 COMMAND ${_test_tgt} "${ACCEL_TPCH_TEST_DB_PATH}")
        set_tests_properties(tpch_q${_q}_aux_smoke_test PROPERTIES
            FIXTURES_REQUIRED tpch_test_db TIMEOUT 300)
    endforeach()
endfunction()



set(_accel_tpch_test_db "${ACCEL_TEST_DB_DIR}/tpch_test.duckdb")
set(ACCEL_TPCH_TEST_DB_PATH "${_accel_tpch_test_db}")
set(_accel_sec_edgar_test_db "${ACCEL_TEST_DB_DIR}/sec_edgar_test.duckdb")

# The test-suite format (eval/harness/suite.hpp): loading and its small pieces.
accel_add_backend_exe(duckdb test_suite_format eval/tests/test_suite_format.cpp)

add_executable(test_registration
    eval/tests/test_registration.cpp
)

target_link_libraries(test_registration
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

accel_add_backend_exe(duckdb test_partitioned_build eval/tests/test_partitioned_build.cpp)
if(BUILD_TESTING)
    add_test(NAME partitioned_build_test COMMAND test_partitioned_build)
endif()

accel_add_backend_exe(duckdb test_shared_incremental_differential eval/tests/test_shared_incremental_differential.cpp)
# Generic test / bench runner for Verus accelerators of the host kit (needs no
# particular accelerator: the Verus pipeline builds it in a tree with none).
add_executable(verus_kit_runner eval/tests/verus_kit/verus_kit_runner.cpp)
target_link_libraries(verus_kit_runner PRIVATE accelerator_codegen duckdb_sibling)

accel_add_backend_exe(duckdb test_duckdb_tpch_differential backends/duckdb/tests/harness/test_duckdb_tpch_differential.cpp)
accel_add_backend_exe(duckdb test_shared_tpch_rf_differential eval/tests/test_shared_tpch_rf_differential.cpp)
accel_add_backend_exe(duckdb test_fallback_state_agree eval/tests/test_fallback_state_agree.cpp)
accel_add_backend_exe(duckdb time_fallback_read eval/tools/time_fallback_read.cpp)
accel_add_backend_exe(duckdb audit_incremental_maintenance eval/tools/audit_incremental_maintenance.cpp)
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

add_executable(test_aux_calibration
    eval/tests/test_aux_calibration.cpp
)

target_link_libraries(test_aux_calibration
    PRIVATE
        accelerator_codegen
)

target_include_directories(test_aux_calibration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)
target_compile_definitions(test_aux_calibration PRIVATE ACCEL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

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

add_executable(bench_sharded_sorted_map eval/bench/aux_state/bench_sharded_sorted_map.cpp)
target_include_directories(bench_sharded_sorted_map PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)
target_link_libraries(bench_sharded_sorted_map PRIVATE Threads::Threads)

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

foreach(_aux_gate_test IN ITEMS test_duckdb_cross_accel test_cross_accel_isolation test_duckdb_aux_multi_connection_stress)
    if(_aux_gate_test STREQUAL "test_cross_accel_isolation")
        set(_aux_gate_source eval/tests/test_cross_accel_isolation.cpp)
    else()
        set(_aux_gate_source backends/duckdb/tests/harness/${_aux_gate_test}.cpp)
    endif()
    add_executable(${_aux_gate_test} ${_aux_gate_source})
    target_link_libraries(${_aux_gate_test} PRIVATE accelerator_codegen duckdb_sibling)
    target_include_directories(${_aux_gate_test} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)
    target_sources(${_aux_gate_test} PRIVATE eval/tests/test_support/cross_accel_helpers.cpp)
    if(_aux_gate_test STREQUAL "test_duckdb_aux_multi_connection_stress")
        target_sources(${_aux_gate_test} PRIVATE eval/tests/test_support/multi_connection_stress.cpp)
    endif()
    add_test(NAME ${_aux_gate_test} COMMAND ${_aux_gate_test})
    if(_aux_gate_test STREQUAL "test_cross_accel_isolation")
        # ~8 minutes alone; much longer under a parallel ctest.
        set_tests_properties(${_aux_gate_test} PROPERTIES TIMEOUT 2400)
    endif()
endforeach()

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

# The host lives as long as the database (eval/tests/test_host_lifetime.cpp).
add_executable(test_host_lifetime eval/tests/test_host_lifetime.cpp)
target_link_libraries(test_host_lifetime PRIVATE accelerator_codegen duckdb_sibling)
target_include_directories(test_host_lifetime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)

accel_add_backend_exe(duckdb benchmark_tpch_stage_c eval/bench/tpch/single-threaded/benchmark_stage_c.cpp)
accel_add_backend_exe(duckdb benchmark_tpch_breakdown eval/bench/tpch/single-threaded/benchmark_tpch_breakdown.cpp)
accel_add_backend_exe(duckdb benchmark_tpch_validated eval/bench/tpch/single-threaded/benchmark_tpch_validated.cpp)
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

set_target_properties(test_registration benchmark_tpch_stage_c benchmark_tpch_breakdown benchmark_tpch_validated PROPERTIES
    BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
    INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
)

# Same as accel_add_backend_exe(duckdb ...), for the multithreaded TPC-H variants (ACCEL_ENABLE_PARALLEL_KERNELS).
function(accel_add_parallel_tpch_exe name)
    accel_add_backend_exe(duckdb ${name} ${ARGN})
    target_compile_definitions(${name} PRIVATE ACCEL_ENABLE_PARALLEL_KERNELS)
    set_target_properties(${name} PROPERTIES
        BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
        INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}")
endfunction()

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
    add_test(NAME test_suite_format COMMAND test_suite_format)
    add_test(NAME shared_aux_state_unit_test COMMAND test_shared_aux_state)
    add_test(NAME aux_calibration_unit_test COMMAND test_aux_calibration)
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
        NAME test_duckdb_tpch_differential_q1_forced_rebuild_partition
        COMMAND test_duckdb_tpch_differential 1)
    set_tests_properties(
        test_duckdb_tpch_differential_q1_forced_rebuild_partition
        PROPERTIES FIXTURES_REQUIRED tpch_test_db TIMEOUT 300
        ENVIRONMENT "ACCEL_TPCH_DB=${_accel_tpch_test_db};ACCEL_FORCE_MAINTENANCE_ACTION=RebuildPartition;ACCEL_AUX_READ_THREADS=8;ACCEL_AUX_BUILD_THREADS=8;ACCEL_AUX_MAINT_THREADS=8")
    # A forced RebuildAll: the read right after the commit (the rebuild may
    # still run) and the one after it settles both match DuckDB.
    foreach(_q 2 7 13 18 21)
        add_test(
            NAME test_duckdb_tpch_differential_q${_q}_forced_rebuild_all
            COMMAND test_duckdb_tpch_differential ${_q})
        set_tests_properties(
            test_duckdb_tpch_differential_q${_q}_forced_rebuild_all
            PROPERTIES FIXTURES_REQUIRED tpch_test_db TIMEOUT 300
            ENVIRONMENT "ACCEL_TPCH_DB=${_accel_tpch_test_db};ACCEL_FORCE_MAINTENANCE_ACTION=RebuildAll")
    endforeach()
    set_tests_properties(test_aux_snapshot_reads_q14_private
        PROPERTIES FIXTURES_REQUIRED tpch_test_db TIMEOUT 300)
    foreach(_q 1 6)
        add_test(NAME test_aux_read_own_writes_q${_q}
                 COMMAND test_aux_read_own_writes "${_accel_tpch_test_db}" ${_q})
        set_tests_properties(test_aux_read_own_writes_q${_q}
            PROPERTIES FIXTURES_REQUIRED tpch_test_db TIMEOUT 300)
    endforeach()
    add_test(NAME test_host_lifetime_q6 COMMAND test_host_lifetime "${_accel_tpch_test_db}" 6)
    set_tests_properties(test_host_lifetime_q6 PROPERTIES FIXTURES_REQUIRED tpch_test_db TIMEOUT 300)
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

# The workload bench and calibrate harnesses (host_bootstrap's, as every target of this file).
# Shared TPC-H aux bench and cost-model calibrator: one binary each, `--query N`.
accel_add_backend_exe(duckdb benchmark_duckdb_tpch_aux
    backends/duckdb/bench/benchmark_duckdb_tpch_aux.cpp
    backends/duckdb/bench/aux_benchmark_sweep_session.cpp
    eval/bench/tpch/single-threaded/tpch_arg_variants.cpp)
accel_add_backend_exe(duckdb calibrate_duckdb_tpch_aux_cost_model
    backends/duckdb/bench/calibrate_duckdb_tpch_aux_cost_model.cpp
    eval/tests/test_support/cross_accel_helpers.cpp
    eval/tests/test_support/multi_connection_stress.cpp)
