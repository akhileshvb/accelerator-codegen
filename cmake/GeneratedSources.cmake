# Collect generated accelerator sources from the per-engine layout:
#   accelerators/{bench}/q{N}/{engine}/base|aux/...
#   backends/{engine}/api/catalog/*.cpp
#
# Set -DACCEL_CODEGEN_ENGINE=<engine> (required to pick catalog + query trees).
# Empty engine or missing trees → no generated query sources (substrate-only).

function(accel_codegen_engine_name OUT_VAR)
    if(DEFINED ACCEL_CODEGEN_ENGINE AND NOT "${ACCEL_CODEGEN_ENGINE}" STREQUAL "")
        set(${OUT_VAR} "${ACCEL_CODEGEN_ENGINE}" PARENT_SCOPE)
    elseif(DEFINED ENV{ACCEL_CODEGEN_ENGINE} AND NOT "$ENV{ACCEL_CODEGEN_ENGINE}" STREQUAL "")
        set(${OUT_VAR} "$ENV{ACCEL_CODEGEN_ENGINE}" PARENT_SCOPE)
    else()
        # Library may still build duckdb host adapters; query sources stay empty
        # until an engine is specified. Do not silently default pipelines.
        set(${OUT_VAR} "" PARENT_SCOPE)
    endif()
endfunction()

function(accel_collect_generated_sources OUT_VAR)
    accel_codegen_engine_name(_engine)
    if(_engine STREQUAL "")
        message(STATUS "accel: ACCEL_CODEGEN_ENGINE unset — skipping generated query sources")
        set(${OUT_VAR} "" PARENT_SCOPE)
        return()
    endif()
    set(_patterns
        "accelerators/*/q*/${_engine}/aux/*.cpp"
    )
    # Engine-neutral per-query code shared by every engine's aux module
    # (accelerators/{bench}/q{N}/common/*.cpp).
    list(APPEND _patterns
        "accelerators/*/q*/common/*.cpp")
    # Base kernels are optional; collect them when present for this engine.
    # Postgres base kernels run inside the module (PostgresAdapter.cmake).
    if(NOT _engine STREQUAL "postgres")
        list(APPEND _patterns
            "accelerators/*/q*/${_engine}/base/single-threaded/*.cpp")
    endif()
    # Catalog for this engine (duckdb catalog also linked via runtime when present).
    if(NOT _engine STREQUAL "duckdb")
        list(APPEND _patterns "backends/${_engine}/api/catalog/*.cpp")
    endif()
    set(_sources "")
    foreach(_pattern IN LISTS _patterns)
        file(GLOB _matches CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${_pattern}")
        list(APPEND _sources ${_matches})
    endforeach()
    list(SORT _sources)
    set(${OUT_VAR} ${_sources} PARENT_SCOPE)
endfunction()

function(accel_collect_generated_parallel_sources OUT_VAR)
    accel_codegen_engine_name(_engine)
    if(_engine STREQUAL "" OR _engine STREQUAL "postgres")
        set(${OUT_VAR} "" PARENT_SCOPE)
        return()
    endif()
    file(GLOB _matches CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/accelerators/*/q*/${_engine}/base/parallel/*.cpp")
    list(SORT _matches)
    set(${OUT_VAR} ${_matches} PARENT_SCOPE)
endfunction()

function(accel_collect_generated_parallel_aux_sources OUT_VAR)
    accel_codegen_engine_name(_engine)
    if(_engine STREQUAL "")
        set(${OUT_VAR} "" PARENT_SCOPE)
        return()
    endif()
    file(GLOB _matches CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/accelerators/*/q*/${_engine}/aux/parallel/*.cpp")
    list(SORT _matches)
    set(${OUT_VAR} ${_matches} PARENT_SCOPE)
endfunction()

function(accel_collect_runtime_sources OUT_VAR)
    accel_codegen_engine_name(_engine)
    set(_sources "")
    # DuckDB host registration/runtime needs duckdb base kernels. Only link when
    # building the duckdb engine (or when engine is unset for substrate-only).
    if(_engine STREQUAL "" OR _engine STREQUAL "duckdb")
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/backends/duckdb/engine/duckdb_scan_adapter.cpp")
            list(APPEND _sources
                backends/duckdb/engine/duckdb_scan_adapter.cpp
                backends/duckdb/api/query_runtime.cpp
                backends/duckdb/api/register.cpp
                backends/duckdb/api/duckdb_host.cpp
                backends/duckdb/api/duckdb_runtime.cpp
                backends/duckdb/api/verus_host.cpp
                backends/duckdb/api/cost_model_path.cpp
                backends/duckdb/engine/scan.cpp
                backends/duckdb/engine/capture.cpp
                backends/duckdb/api/shared_query_engine.cpp
                eval/harness/differential.cpp
                eval/harness/cost_calibration.cpp
                eval/harness/tpch_rf.cpp
            )
            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/backends/duckdb/api/catalog/query_catalog.cpp")
                list(APPEND _sources backends/duckdb/api/catalog/query_catalog.cpp)
            endif()
        endif()
    else()
        # Non-duckdb engines: omit DuckDB harness (references DuckdbRuntime).
        if(ACCELERATOR_CODEGEN_BUILD_MYSQL AND EXISTS
           "${CMAKE_CURRENT_SOURCE_DIR}/backends/mysql/engine/scan_helper.cpp")
            list(APPEND _sources backends/mysql/engine/scan_helper.cpp)
        endif()
    endif()
    set(${OUT_VAR} "${_sources}" PARENT_SCOPE)
endfunction()

function(accel_collect_stateful_sources OUT_VAR)
    accel_codegen_engine_name(_engine)
    set(_sources
        substrate/aux/state/invalidation_plan.cpp
        substrate/aux/state/versioned_aux_store.cpp
        substrate/aux/state/aux_state_footprint.cpp
        substrate/aux/state/verus_aux_store.cpp
        substrate/verus/kit/verus_kit.cpp
        substrate/aux/core/aux_engine_services.cpp
        substrate/aux/core/aux_maintenance_policy.cpp
        substrate/aux/core/aux_maintenance_decision_log.cpp
        substrate/aux/core/aux_calibration.cpp
        substrate/aux/core/aux_cost_model_path.cpp
        substrate/aux/core/aux_store_diagnostics.cpp
        substrate/aux/core/aux_execute_stats.cpp
        substrate/aux/runtime/accelerator_runtime.cpp
        substrate/aux/runtime/columnar_batch.cpp
        substrate/aux/runtime/wire.cpp
        substrate/aux/runtime/runtime_host.cpp
        substrate/aux/runtime/sequenced_applier.cpp
        substrate/aux/runtime/thread_budget.cpp
        substrate/aux/maintenance/maintenance_action.cpp
        substrate/aux/maintenance/action_selector.cpp
    )
    if((_engine STREQUAL "" OR _engine STREQUAL "duckdb") AND
       EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/backends/duckdb/api/duckdb_aux_table_functions.cpp")
        list(APPEND _sources
            backends/duckdb/api/duckdb_aux_table_functions.cpp
            backends/duckdb/api/sql_fallback.cpp
            backends/duckdb/engine/duckdb_aux_backend.cpp
        )
    endif()
    if(ACCELERATOR_CODEGEN_BUILD_MYSQL)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/backends/mysql/engine/mysql_statement_capture.cpp")
            list(APPEND _sources backends/mysql/engine/mysql_statement_capture.cpp)
        endif()
    endif()
    set(${OUT_VAR} "${_sources}" PARENT_SCOPE)
endfunction()
