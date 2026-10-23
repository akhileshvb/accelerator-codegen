# PostgreSQL backend: the extension module (accelerator_codegen_pg), built
# against the server SDK. The Docker pipeline builds it inside the Postgres host container
# (backends/postgres/host.sh build). The writer-side test modules and
# the shared-memory unit tests live here too.

# Unit tests of the shared-memory transport: pure C++/Linux, no Postgres deps.
# The sibling DuckDB: harnesses and generated query code use it as the reference engine.
accel_require_duckdb()

find_package(Threads REQUIRED)
add_executable(test_shm_ring
    backends/postgres/tests/test_shm_ring.cpp
    backends/postgres/state/shm_ring.cpp
)
add_executable(test_shard_segment
    backends/postgres/tests/test_shard_segment.cpp
    backends/postgres/state/shard_segment.cpp
    backends/postgres/state/shm_ring.cpp
)
add_executable(test_pg_numeric backends/postgres/tests/test_pg_numeric.cpp)
foreach(_target IN ITEMS test_shm_ring test_shard_segment test_pg_numeric)
    target_include_directories(${_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)
    target_link_libraries(${_target} PRIVATE Threads::Threads)
endforeach()
if(BUILD_TESTING)
    add_test(NAME shm_ring_test COMMAND test_shm_ring)
    add_test(NAME shard_segment_test COMMAND test_shard_segment)
    add_test(NAME pg_numeric_test COMMAND test_pg_numeric)
endif()

accel_backend_library(
    CATALOG backends/postgres/api/catalog
    AUX aux/*.cpp
)

# Discovery rejects client-only pg_config.
include("${CMAKE_CURRENT_LIST_DIR}/FindAccelPostgreSQL.cmake")
if(NOT ACCEL_PG_FOUND)
    # Host cmake in Docker pipeline mode must not FATAL: the module builds
    # inside accel_pg_test, where configures always pass PG_CONFIG.
    if("$ENV{ACCEL_PG_DOCKER}" STREQUAL "1")
        message(STATUS
            "accel: BUILD_POSTGRES ON but no host SDK (ACCEL_PG_DOCKER=1); "
            "skipping host MODULE — build inside Docker instead")
        return()
    endif()
    message(FATAL_ERROR
        "ACCELERATOR_CODEGEN_BUILD_POSTGRES=ON but no orchestrator-resolved "
        "PostgreSQL server SDK was provided. For the Docker pipeline use "
        "backends/postgres/host.sh build (in-container build). "
        "Or export PG_CONFIG to a complete install (postgres.h + initdb).")
endif()

target_include_directories(accelerator_codegen PRIVATE ${ACCEL_PG_INCLUDEDIR} ${ACCEL_PG_INCLUDEDIR_SERVER})

find_library(ACCEL_PG_LIBPQ pq PATHS "${ACCEL_PG_LIBDIR}" NO_DEFAULT_PATH)
add_library(accel_backend_postgres INTERFACE)
target_include_directories(accel_backend_postgres INTERFACE ${ACCEL_PG_INCLUDEDIR} ${ACCEL_PG_INCLUDEDIR_SERVER})
target_link_libraries(accel_backend_postgres INTERFACE ${ACCEL_PG_LIBPQ})
set_target_properties(accel_backend_postgres PROPERTIES ACCEL_BACKEND_MODULE accelerator_codegen_pg)

# Shared settings for the production and test Postgres MODULEs.
function(_accel_pg_module_setup _target)
  set_target_properties(${_target} PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    BUILD_RPATH "${ACCEL_PG_LIBDIR}"
    INSTALL_RPATH "${ACCEL_PG_LIBDIR}"
  )
  target_link_libraries(${_target}
    PRIVATE
      accelerator_codegen
      duckdb_sibling
  )
  target_include_directories(${_target}
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators
      ${ACCEL_PG_INCLUDEDIR}
      ${ACCEL_PG_INCLUDEDIR_SERVER}
  )
  target_compile_definitions(${_target} PRIVATE
    ACCEL_PG_MODULE
    ACCELERATOR_CODEGEN_POSTGRES_EXTENSION
  )
endfunction()

# The writer side (S1-S4): capture, commit lifecycle, registry and epochs.
set(_PG_WRITER_SOURCES
  backends/postgres/engine/accelerator_tables.cpp
  backends/postgres/engine/postgres_xact_callbacks.cpp
  backends/postgres/engine/transition_capture.cpp
  backends/postgres/state/messages.cpp
  backends/postgres/state/shared.cpp
)

# Shard workers (state/shard_*.cpp: the transport and server threads are
# Postgres-free, the rest is the worker, its slots and the reader side), the
# accelerator registry the per-query code registers in, and the base runtime:
# parallel heap scans in Postgres worker processes, decoded into columnar
# batches for the per-query base kernels (also used by the build scans of the
# aux accelerators).
set(_pg_ext_sources
  ${_PG_WRITER_SOURCES}
  backends/postgres/engine/extension_engine.cpp
  backends/postgres/engine/scan_helper.cpp
  backends/postgres/api/extension_api.cpp
  backends/postgres/api/runtime.cpp
  backends/postgres/api/accelerators.cpp
  backends/postgres/state/shm_ring.cpp
  backends/postgres/state/shard_segment.cpp
  backends/postgres/state/shard_server.cpp
  backends/postgres/state/shard_slots.cpp
  backends/postgres/state/shard_worker.cpp
  backends/postgres/state/shard_gucs.cpp
  backends/postgres/state/remote_read.cpp
  backends/postgres/api/base/runtime_common.cpp
  backends/postgres/api/base/runtime_pg.cpp)

# Per-query module code (SPI scans, PG_FUNCTION shims, base kernels). Each
# query registers itself through static registrars (postgres/runtime.hpp);
# no query is named here, and a tree without any q<N> directory still builds.
accel_glob_sources(_pg_query_sources "accelerators/*/q*/postgres/extension/*.cpp")
accel_glob_sources(_pg_base_sources
  "accelerators/*/q*/postgres/base/*.cpp"
  "accelerators/*/q*/postgres/base/*/*.cpp")
accel_apply_base_copts(${_pg_base_sources})

add_library(accelerator_codegen_pg MODULE ${_pg_ext_sources} ${_pg_query_sources} ${_pg_base_sources})
_accel_pg_module_setup(accelerator_codegen_pg)

# Build-scan test: the toy module through the partitioned scan, in SQL
# (scan_test_partitioned); the workers run the production module's tasks.
add_library(accelerator_codegen_pg_scan_test MODULE backends/postgres/tests/harness/test_scan_module.cpp)
_accel_pg_module_setup(accelerator_codegen_pg_scan_test)

# Writer-side tests: the writer sources against a recording stub of the shard
# worker (test_writer_module.cpp) and fake accelerators, driven over libpq by
# test_postgres_writer (needs a server with the module installed).
add_library(accelerator_codegen_pg_writer_test MODULE
  ${_PG_WRITER_SOURCES} backends/postgres/tests/test_writer_module.cpp)
_accel_pg_module_setup(accelerator_codegen_pg_writer_test)
add_executable(test_postgres_writer backends/postgres/tests/test_postgres_writer.cpp)
target_link_libraries(test_postgres_writer PRIVATE ${ACCEL_PG_LIBPQ} Threads::Threads)
target_include_directories(test_postgres_writer PRIVATE ${ACCEL_PG_INCLUDEDIR})

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
