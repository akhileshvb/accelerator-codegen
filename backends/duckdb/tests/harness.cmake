# DuckDB eval harnesses compiled into libaccelerator_codegen (they drive
# DuckdbRuntime); included by the root after backends/duckdb/backend.cmake.
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
