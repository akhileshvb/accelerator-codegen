# Engine selection and the substrate sources of libaccelerator_codegen.
# Engine sources attach through the backend interface (Backend.cmake).
#
# -DACCEL_CODEGEN_ENGINE=<engine> picks the per-query trees
# (accelerators/{bench}/q{N}/{engine}/...). Empty: no per-query sources.

function(accel_codegen_engine_name OUT_VAR)
    if(DEFINED ACCEL_CODEGEN_ENGINE AND NOT "${ACCEL_CODEGEN_ENGINE}" STREQUAL "")
        set(${OUT_VAR} "${ACCEL_CODEGEN_ENGINE}" PARENT_SCOPE)
    elseif(DEFINED ENV{ACCEL_CODEGEN_ENGINE} AND NOT "$ENV{ACCEL_CODEGEN_ENGINE}" STREQUAL "")
        set(${OUT_VAR} "$ENV{ACCEL_CODEGEN_ENGINE}" PARENT_SCOPE)
    else()
        # Query sources stay empty until an engine is specified. Do not silently default pipelines.
        set(${OUT_VAR} "" PARENT_SCOPE)
    endif()
endfunction()

function(accel_collect_stateful_sources OUT_VAR)
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
    set(${OUT_VAR} "${_sources}" PARENT_SCOPE)
endfunction()
