# Collect generated accelerator sources from the query-centric layout.

function(accel_collect_generated_sources OUT_VAR)
    set(_patterns
        "src/generated/tpch/q*/base/single-threaded/*.cpp"
        "src/generated/tpch/q*/aux/*.cpp"
        "src/generated/sec_edgar/q*/base/single-threaded/*.cpp"
        "src/generated/sec_edgar/q*/aux/*.cpp"
        "src/generated/sec_edgar/runtime.cpp"
        "src/generated/catalog/*.cpp"
    )
    set(_sources "")
    foreach(_pattern IN LISTS _patterns)
        file(GLOB _matches CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${_pattern}")
        list(APPEND _sources ${_matches})
    endforeach()
    list(SORT _sources)
    set(${OUT_VAR} ${_sources} PARENT_SCOPE)
endfunction()

function(accel_collect_generated_parallel_sources OUT_VAR)
    file(GLOB _matches CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/tpch/q*/base/parallel/*.cpp")
    list(SORT _matches)
    set(${OUT_VAR} ${_matches} PARENT_SCOPE)
endfunction()

function(accel_collect_generated_parallel_aux_sources OUT_VAR)
    file(GLOB _matches CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/sec_edgar/q*/aux/parallel/*.cpp")
    list(SORT _matches)
    set(${OUT_VAR} ${_matches} PARENT_SCOPE)
endfunction()

function(accel_collect_runtime_sources OUT_VAR)
    set(_sources
        src/runtime/duckdb/duckdb_scan_adapter.cpp
        src/runtime/duckdb/query_runtime.cpp
        src/runtime/duckdb/register.cpp
        src/runtime/duckdb/duckdb_runtime.cpp
        src/runtime/duckdb/tpch_rf.cpp
        src/runtime/catalog/shared_query_engine.cpp
        src/harness/differential.cpp
        src/harness/cost_calibration.cpp
        src/harness/aux_state_footprint.cpp
    )
    set(${OUT_VAR} ${_sources} PARENT_SCOPE)
endfunction()

function(accel_collect_stateful_sources OUT_VAR)
    set(_sources
        src/stateful/state/invalidation_plan.cpp
        src/stateful/state/partitioned_state_store.cpp
        src/stateful/core/aux_registry.cpp
        src/stateful/core/aux_engine_services.cpp
        src/stateful/core/aux_maintenance_policy.cpp
        src/stateful/core/aux_maintenance_decision_log.cpp
        src/stateful/core/aux_calibration.cpp
        src/stateful/core/aux_store_diagnostics.cpp
        src/stateful/maintenance/maintenance_action.cpp
        src/stateful/maintenance/action_selector.cpp
        src/stateful/duckdb/duckdb_aux_table_functions.cpp
        src/stateful/duckdb/duckdb_aux_backend.cpp
        src/stateful/duckdb/duckdb_state_store.cpp
        src/stateful/postgres/postgres_aux_backend.cpp
        src/stateful/postgres/postgres_mutation_capture.cpp
        src/stateful/postgres/postgres_session_gucs.cpp
        src/stateful/postgres/postgres_state_store.cpp
        src/stateful/postgres/postgres_xact_callbacks.cpp
    )
    set(${OUT_VAR} ${_sources} PARENT_SCOPE)
endfunction()

function(accel_collect_postgres_generated_sources OUT_VAR)
    set(_patterns
        "src/generated/postgres/*.cpp"
    )
    set(_sources "")
    foreach(_pattern IN LISTS _patterns)
        file(GLOB _matches CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${_pattern}")
        list(APPEND _sources ${_matches})
    endforeach()
    list(SORT _sources)
    set(${OUT_VAR} ${_sources} PARENT_SCOPE)
endfunction()

function(accel_collect_postgres_runtime_sources OUT_VAR)
    set(_sources
        src/runtime/postgres/scan_adapter.cpp
        src/runtime/postgres/kernel_executor.cpp
        src/runtime/postgres/bind_helper.cpp
        src/runtime/postgres/extension.cpp
        src/runtime/postgres/query_functions.cpp
    )
    set(${OUT_VAR} ${_sources} PARENT_SCOPE)
endfunction()

function(accel_collect_postgres_stateful_sources OUT_VAR)
    set(_sources
        src/stateful/state/invalidation_plan.cpp
        src/stateful/state/partitioned_state_store.cpp
        src/stateful/core/aux_registry.cpp
        src/stateful/core/aux_engine_services.cpp
        src/stateful/core/aux_maintenance_policy.cpp
        src/stateful/core/aux_maintenance_decision_log.cpp
        src/stateful/core/aux_calibration.cpp
        src/stateful/maintenance/action_selector.cpp
        src/stateful/postgres/postgres_aux_backend.cpp
        src/stateful/postgres/postgres_mutation_capture.cpp
        src/stateful/postgres/postgres_session_gucs.cpp
        src/stateful/postgres/postgres_state_store.cpp
        src/stateful/postgres/postgres_xact_callbacks.cpp
    )
    set(${OUT_VAR} ${_sources} PARENT_SCOPE)
endfunction()
