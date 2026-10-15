# Postgres engine adapter — build the MODULE (accelerator_codegen_pg) and the
# writer-side test module inside the accel_pg_test Docker container.

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

function(accel_build_postgres_extension)
  set(_pg_ext_sources ${_PG_WRITER_SOURCES})

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/backends/postgres/engine/extension_engine.cpp")
    list(APPEND _pg_ext_sources backends/postgres/engine/extension_engine.cpp)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/backends/postgres/engine/scan_helper.cpp")
    list(APPEND _pg_ext_sources backends/postgres/engine/scan_helper.cpp)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/backends/postgres/api/extension_api.cpp")
    list(APPEND _pg_ext_sources backends/postgres/api/extension_api.cpp)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/backends/postgres/api/runtime.cpp")
    list(APPEND _pg_ext_sources backends/postgres/api/runtime.cpp)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/backends/postgres/engine/postgres_xact_callbacks.cpp")
    list(APPEND _pg_ext_sources backends/postgres/engine/postgres_xact_callbacks.cpp)
  endif()

  # Shard workers (state/shard_*.cpp: the transport and server threads are
  # Postgres-free, the rest is the worker, its slots and the reader side) and
  # the accelerator registry the per-query code registers in.
  list(APPEND _pg_ext_sources
    backends/postgres/api/accelerators.cpp
    backends/postgres/state/shm_ring.cpp
    backends/postgres/state/shard_segment.cpp
    backends/postgres/state/shard_server.cpp
    backends/postgres/state/shard_slots.cpp
    backends/postgres/state/shard_worker.cpp
    backends/postgres/state/shard_gucs.cpp
    backends/postgres/state/remote_read.cpp)

  # Base runtime: parallel heap scans in Postgres worker processes, decoded
  # into columnar batches for the per-query base kernels (also used by the
  # build scans of the aux accelerators).
  list(APPEND _pg_ext_sources
    backends/postgres/api/base/runtime_common.cpp
    backends/postgres/api/base/runtime_pg.cpp)

  # Per-query module code (SPI scans, PG_FUNCTION shims, base kernels). Each
  # query registers itself through static registrars (postgres/runtime.hpp);
  # no query is named here, and a tree without any q<N> directory still builds.
  file(GLOB _pg_query_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/accelerators/*/q*/postgres/extension/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/accelerators/*/q*/postgres/base/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/accelerators/*/q*/postgres/base/*/*.cpp")
  list(SORT _pg_query_sources)
  accel_apply_base_copts(${_pg_query_sources})

  if(_pg_ext_sources)
    add_library(accelerator_codegen_pg MODULE ${_pg_ext_sources} ${_pg_query_sources})
    _accel_pg_module_setup(accelerator_codegen_pg)
  endif()

  # Writer-side tests: the writer sources against a recording stub of the shard
  # worker (test_writer_module.cpp) and fake accelerators, driven over libpq by
  # test_postgres_writer (needs a server with the module installed).
  add_library(accelerator_codegen_pg_writer_test MODULE
    ${_PG_WRITER_SOURCES} backends/postgres/tests/test_writer_module.cpp)
  _accel_pg_module_setup(accelerator_codegen_pg_writer_test)
  find_library(_PG_WRITER_LIBPQ pq PATHS "${ACCEL_PG_LIBDIR}" NO_DEFAULT_PATH)
  add_executable(test_postgres_writer backends/postgres/tests/test_postgres_writer.cpp)
  find_package(Threads REQUIRED)
  target_link_libraries(test_postgres_writer PRIVATE ${_PG_WRITER_LIBPQ} Threads::Threads)
  target_include_directories(test_postgres_writer PRIVATE ${ACCEL_PG_INCLUDEDIR})
endfunction()
