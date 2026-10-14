# Collect generated accelerator sources from the per-engine layout:
#   src/generated/{bench}/q{N}/{engine}/base|aux/...
#   src/generated/catalog/{engine}/*.cpp
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
        "src/generated/*/q*/${_engine}/aux/*.cpp"
    )
    # Engine-neutral per-query code shared by every engine's aux module
    # (src/generated/{bench}/q{N}/common/*.cpp).
    list(APPEND _patterns
        "src/generated/*/q*/common/*.cpp")
    # Base kernels are optional; collect them when present for this engine.
    list(APPEND _patterns
        "src/generated/*/q*/${_engine}/base/single-threaded/*.cpp")
    # Catalog for this engine (duckdb catalog also linked via runtime when present).
    if(NOT _engine STREQUAL "duckdb")
        list(APPEND _patterns "src/generated/catalog/${_engine}/*.cpp")
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
    if(_engine STREQUAL "")
        set(${OUT_VAR} "" PARENT_SCOPE)
        return()
    endif()
    file(GLOB _matches CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/*/q*/${_engine}/base/parallel/*.cpp")
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
        "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/*/q*/${_engine}/aux/parallel/*.cpp")
    list(SORT _matches)
    set(${OUT_VAR} ${_matches} PARENT_SCOPE)
endfunction()

function(accel_collect_runtime_sources OUT_VAR)
    accel_codegen_engine_name(_engine)
    set(_sources "")
    # DuckDB host registration/runtime needs duckdb base kernels. Only link when
    # building the duckdb engine (or when engine is unset for substrate-only).
    if(_engine STREQUAL "" OR _engine STREQUAL "duckdb")
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/duckdb/duckdb_scan_adapter.cpp")
            list(APPEND _sources
                src/runtime/duckdb/duckdb_scan_adapter.cpp
                src/runtime/duckdb/query_runtime.cpp
                src/runtime/duckdb/register.cpp
                src/runtime/duckdb/duckdb_runtime.cpp
                src/runtime/catalog/shared_query_engine.cpp
                src/harness/differential.cpp
                src/harness/cost_calibration.cpp
                src/harness/aux_state_footprint.cpp
                src/harness/tpch_rf.cpp
            )
            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/catalog/duckdb/query_catalog.cpp")
                list(APPEND _sources src/generated/catalog/duckdb/query_catalog.cpp)
            endif()
        endif()
    else()
        # Non-duckdb engines: omit DuckDB harness (references DuckdbRuntime).
        if(ACCELERATOR_CODEGEN_BUILD_MYSQL AND EXISTS
           "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/mysql/scan_helper.cpp")
            list(APPEND _sources src/runtime/mysql/scan_helper.cpp)
        endif()
    endif()
    set(${OUT_VAR} "${_sources}" PARENT_SCOPE)
endfunction()

function(accel_collect_stateful_sources OUT_VAR)
    accel_codegen_engine_name(_engine)
    set(_sources
        src/stateful/state/invalidation_plan.cpp
        src/stateful/state/versioned_aux_store.cpp
        src/stateful/state/verus_aux_store.cpp
        src/verus_kit/verus_kit.cpp
        src/stateful/core/aux_registry.cpp
        src/stateful/core/aux_engine_services.cpp
        src/stateful/core/aux_maintenance_policy.cpp
        src/stateful/core/aux_maintenance_decision_log.cpp
        src/stateful/core/aux_calibration.cpp
        src/stateful/core/aux_store_diagnostics.cpp
        src/stateful/core/aux_execute_stats.cpp
        src/stateful/maintenance/maintenance_action.cpp
        src/stateful/maintenance/action_selector.cpp
    )
    if((_engine STREQUAL "" OR _engine STREQUAL "duckdb") AND
       EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/stateful/duckdb/duckdb_aux_table_functions.cpp")
        list(APPEND _sources
            src/stateful/duckdb/duckdb_aux_table_functions.cpp
            src/stateful/duckdb/duckdb_aux_backend.cpp
            src/stateful/duckdb/duckdb_state_store.cpp
        )
    endif()
    if(ACCELERATOR_CODEGEN_BUILD_POSTGRES)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/stateful/postgres/postgres_aux_backend.cpp")
            list(APPEND _sources
                src/stateful/postgres/postgres_aux_backend.cpp
            )
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/stateful/postgres/postgres_state_store.cpp")
            list(APPEND _sources
                src/stateful/postgres/postgres_state_store.cpp
            )
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/stateful/postgres/postgres_mutation_capture.cpp")
            list(APPEND _sources
                src/stateful/postgres/postgres_mutation_capture.cpp
            )
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/stateful/postgres/snapshot_token.cpp")
            list(APPEND _sources
                src/stateful/postgres/snapshot_token.cpp
            )
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/stateful/postgres/aux_mutation_epoch.cpp")
            list(APPEND _sources
                src/stateful/postgres/aux_mutation_epoch.cpp
            )
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/stateful/postgres/aux_mutation_epoch.cpp")
            list(APPEND _sources
                src/stateful/postgres/aux_mutation_epoch.cpp
            )
        endif()
    endif()
    if(ACCELERATOR_CODEGEN_BUILD_MYSQL)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/stateful/mysql/mysql_aux_backend.cpp")
            list(APPEND _sources src/stateful/mysql/mysql_aux_backend.cpp)
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/stateful/mysql/mysql_state_store.cpp")
            list(APPEND _sources src/stateful/mysql/mysql_state_store.cpp)
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/stateful/mysql/mysql_mutation_capture.cpp")
            list(APPEND _sources src/stateful/mysql/mysql_mutation_capture.cpp)
        endif()
    endif()
    set(${OUT_VAR} "${_sources}" PARENT_SCOPE)
endfunction()
