# DuckDB backend: the in-process host, compiled into libaccelerator_codegen.

accel_backend_library(
    HOST
        backends/duckdb/api/duckdb_scan_adapter.cpp
        backends/duckdb/api/scan.cpp
        backends/duckdb/engine/capture.cpp
        backends/duckdb/api/duckdb_aux_backend.cpp
        backends/duckdb/api/query_runtime.cpp
        backends/duckdb/api/register.cpp
        backends/duckdb/api/duckdb_host.cpp
        backends/duckdb/api/duckdb_runtime.cpp
        backends/duckdb/api/verus_host.cpp
        backends/duckdb/api/cost_model_path.cpp
        backends/duckdb/api/shared_query_engine.cpp
        backends/duckdb/api/duckdb_aux_table_functions.cpp
        backends/duckdb/api/sql_fallback.cpp
    CATALOG backends/duckdb/api/catalog
    AUX aux/*.cpp
    BASE base/single-threaded/*.cpp
)

add_library(accel_backend_duckdb INTERFACE)

accel_codegen_engine_name(_duckdb_query_engine)

option(ACCEL_ENABLE_PARALLEL_KERNELS "Build multithreaded TPC-H accelerator variants (accel_tpch_q*_parallel)" OFF)
if(ACCEL_ENABLE_PARALLEL_KERNELS)
    target_compile_definitions(accelerator_codegen PUBLIC ACCEL_ENABLE_PARALLEL_KERNELS)
    # Q3 shared __int128 revenue CAS uses libatomic (__atomic_*_16).
    target_link_libraries(accelerator_codegen PRIVATE atomic)
    if(_duckdb_query_engine STREQUAL "duckdb")
        accel_glob_sources(_duckdb_parallel_sources "accelerators/*/q*/duckdb/base/parallel/*.cpp")
        accel_apply_base_copts(${_duckdb_parallel_sources})
    endif()
    target_sources(accelerator_codegen PRIVATE
        backends/duckdb/api/parallel/parallel_runtime.cpp
        ${_duckdb_parallel_sources}
    )
endif()

option(ACCEL_ENABLE_PARALLEL_AUX "Build multithreaded SEC EDGAR aux kernels (accel_sec_edgar_q*_aux_parallel)" OFF)
if(ACCEL_ENABLE_PARALLEL_AUX)
    target_compile_definitions(accelerator_codegen PUBLIC ACCEL_ENABLE_PARALLEL_AUX)
    if(_duckdb_query_engine STREQUAL "duckdb")
        accel_glob_sources(_duckdb_parallel_aux_sources "accelerators/*/q*/duckdb/aux/parallel/*.cpp")
    endif()
    target_sources(accelerator_codegen PRIVATE
        backends/duckdb/api/parallel/aux_parallel_scan.cpp
        ${_duckdb_parallel_aux_sources}
    )
    if(NOT ACCEL_ENABLE_PARALLEL_KERNELS)
        target_sources(accelerator_codegen PRIVATE backends/duckdb/api/parallel/parallel_runtime.cpp)
    endif()
endif()

# Unit tests of the backend's own state.
accel_add_backend_exe(duckdb test_shared_aux_state backends/duckdb/tests/test_shared_aux_state.cpp)
accel_add_backend_exe(duckdb test_wide_types backends/duckdb/tests/test_wide_types.cpp)
accel_add_backend_exe(duckdb test_versioned_aux_state backends/duckdb/tests/test_versioned_aux_state.cpp)
if(BUILD_TESTING)
    add_test(NAME wide_types_unit_test COMMAND test_wide_types)
    add_test(NAME versioned_aux_state_unit_test COMMAND test_versioned_aux_state)
endif()
