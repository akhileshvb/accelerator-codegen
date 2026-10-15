# Postgres engine adapter — build the MODULE (accelerator_codegen_pg), its
# test-only variant (accelerator_codegen_pg_test) and the engine_backend layer
# harnesses inside the accel_pg_test Docker container.
#
# This file grows with each engine_backend layer. The store layer only needs
# the shared library; later layers add extension sources and harnesses.

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

function(accel_build_postgres_extension)
  set(_pg_ext_sources "")

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

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/backends/postgres/engine/transition_capture.cpp")
    list(APPEND _pg_ext_sources backends/postgres/engine/transition_capture.cpp)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/backends/postgres/engine/postgres_bridge.cpp")
    list(APPEND _pg_ext_sources backends/postgres/engine/postgres_bridge.cpp)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/backends/postgres/engine/postgres_xact_callbacks.cpp")
    list(APPEND _pg_ext_sources backends/postgres/engine/postgres_xact_callbacks.cpp)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/backends/postgres/engine/snapshot_token.cpp")
    list(APPEND _pg_ext_sources backends/postgres/engine/snapshot_token.cpp)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/backends/postgres/state/aux_server.cpp")
    list(APPEND _pg_ext_sources backends/postgres/state/aux_server.cpp)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/backends/postgres/state/aux_sync.cpp")
    list(APPEND _pg_ext_sources backends/postgres/state/aux_sync.cpp)
  endif()

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/backends/postgres/state/aux_mutation_epoch.cpp")
    list(APPEND _pg_ext_sources backends/postgres/state/aux_mutation_epoch.cpp)
  endif()

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

  # Test-only MODULE for the engine_backend layer gates: the backend sources
  # above (no per-query code) plus the gate-test UDFs (toy txn / mutations /
  # scan / runtime / visibility bridges) from backends/postgres/tests/.
  # The layer harnesses LOAD accelerator_codegen_pg_test; the production
  # accelerator_codegen_pg never contains these functions.
  set(_pg_test_sources "")
  foreach(_src IN ITEMS
      backends/postgres/tests/extension_test_engine.cpp)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
      list(APPEND _pg_test_sources ${_src})
    endif()
  endforeach()
  if(_pg_ext_sources AND _pg_test_sources)
    add_library(accelerator_codegen_pg_test MODULE ${_pg_ext_sources} ${_pg_test_sources})
    _accel_pg_module_setup(accelerator_codegen_pg_test)
  endif()
endfunction()
