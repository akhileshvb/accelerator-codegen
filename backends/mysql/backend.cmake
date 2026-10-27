# MySQL backend: the audit/UDF plugin (accelerator_codegen_mysql), built in
# the Docker pipeline inside the MySQL host container (backends/mysql/host.sh build).
# The plugin SDK headers are optional: without them the store/aux layers and the
# standalone harnesses still build.
# The sibling DuckDB: harnesses and generated query code use it as the reference engine.
accel_require_duckdb()

option(ACCEL_MYSQL_TEST_UDFS
       "Build the gate-test MySQL plugin variant (adds backends/mysql/tests/mysql_bridge_test_udfs.cpp)" OFF)

include("${CMAKE_CURRENT_LIST_DIR}/FindAccelMySQL.cmake")
if(NOT ACCEL_MYSQL_FOUND)
    message(STATUS
        "accel: BUILD_MYSQL ON without mysql/plugin.h — "
        "standalone harnesses can still link accelerator_codegen")
endif()

# The scan helper is part of libaccelerator_codegen when it is built for the
# mysql engine; for any other engine the harnesses get it from a small static
# library. None of it needs the plugin SDK.
accel_codegen_engine_name(_mysql_query_engine)
set(_mysql_host_sources backends/mysql/engine/scan_helper.cpp)
accel_backend_library(
    HOST backends/mysql/engine/mysql_statement_capture.cpp
    CATALOG backends/mysql/api/catalog
    AUX aux/*.cpp
    BASE base/single-threaded/*.cpp
)
add_library(accel_backend_mysql INTERFACE)
if(_mysql_query_engine STREQUAL "mysql")
    target_sources(accelerator_codegen PRIVATE ${_mysql_host_sources})
else()
    add_library(accelerator_codegen_mysql_host STATIC ${_mysql_host_sources} backends/mysql/api/catalog/query_catalog.cpp)
    target_include_directories(accelerator_codegen_mysql_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)
    target_compile_features(accelerator_codegen_mysql_host PUBLIC cxx_std_17)
    target_link_libraries(accel_backend_mysql INTERFACE accelerator_codegen_mysql_host)
endif()
if(ACCEL_MYSQL_FOUND AND ACCEL_MYSQL_INCLUDEDIR)
    target_include_directories(accel_backend_mysql INTERFACE ${ACCEL_MYSQL_INCLUDEDIR})
endif()
set_target_properties(accel_backend_mysql PROPERTIES ACCEL_BACKEND_MODULE accelerator_codegen_mysql)

set(_mysql_plugin_sources
    backends/mysql/api/mysql_plugin.cpp
    backends/mysql/api/runtime.cpp
    backends/mysql/api/mysql_host.cpp
    backends/mysql/engine/mysql_bridge.cpp
    backends/mysql/engine/mysql_session.cpp
)

# Test plugin variant: adds the gate-test UDFs (toy txn bridge, mutation
# inspection, scan / runtime drivers that create and seed toy tables). The
# same plugin file name and audit plugin are kept so the layer harnesses
# load it exactly like the production plugin; only the gates build it
# (backends/mysql/host.sh build, with the test-UDF variant requested).
if(ACCEL_MYSQL_TEST_UDFS)
    list(APPEND _mysql_plugin_sources backends/mysql/tests/mysql_bridge_test_udfs.cpp)
    message(STATUS "accel: MySQL plugin variant = test (gate-test UDFs included)")
else()
    message(STATUS "accel: MySQL plugin variant = production")
endif()

# Per-query plugin code (scans, runtimes, UDF shims): every query registers
# itself through the backend registries (backends/mysql/api/runtime.hpp), so
# the backend builds with or without any of these directories.
accel_glob_sources(_mysql_query_plugin_sources "accelerators/*/q*/mysql/plugin/*.cpp")

add_library(accelerator_codegen_mysql MODULE ${_mysql_plugin_sources} ${_mysql_query_plugin_sources})
set_target_properties(accelerator_codegen_mysql PROPERTIES
    PREFIX ""
    SUFFIX ".so"
)
target_link_libraries(accelerator_codegen_mysql
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)
target_include_directories(accelerator_codegen_mysql
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators
)
if(ACCEL_MYSQL_FOUND AND ACCEL_MYSQL_INCLUDEDIR)
    target_include_directories(accelerator_codegen_mysql PRIVATE ${ACCEL_MYSQL_INCLUDEDIR})
endif()
target_compile_definitions(accelerator_codegen_mysql PRIVATE
    ACCEL_MYSQL_PLUGIN
    ACCELERATOR_CODEGEN_MYSQL_PLUGIN
)

# Optional in-process scan path: the MySQL C API (libmysqlclient or MariaDB
# Connector/C; the API used is the common subset: mysql_real_connect,
# mysql_real_query, mysql_use_result, mysql_fetch_row), loaded with dlopen
# (the headers found here only supply the types). Without it the host
# layer streams scans through the mysql CLI and cannot build from a single
# consistent snapshot. Search: ACCEL_MYSQL_CLIENT_ROOT (cmake var or env),
# /opt/accel-mysql-client/usr, then the system.
set(_client_roots "")
if(ACCEL_MYSQL_CLIENT_ROOT)
  list(APPEND _client_roots "${ACCEL_MYSQL_CLIENT_ROOT}")
endif()
if(DEFINED ENV{ACCEL_MYSQL_CLIENT_ROOT} AND NOT "$ENV{ACCEL_MYSQL_CLIENT_ROOT}" STREQUAL "")
  list(APPEND _client_roots "$ENV{ACCEL_MYSQL_CLIENT_ROOT}")
endif()
list(APPEND _client_roots /opt/accel-mysql-client/usr /usr)
set(_client_inc "")
set(_client_lib "")
foreach(_root IN LISTS _client_roots)
  foreach(_sub include/mysql include/mariadb include)
    if(NOT _client_inc AND EXISTS "${_root}/${_sub}/mysql.h")
      set(_client_inc "${_root}/${_sub}")
      set(_client_root "${_root}")
    endif()
  endforeach()
  if(_client_inc AND _root STREQUAL _client_root AND NOT _client_lib)
    foreach(_name mysqlclient mariadb)
      if(NOT _client_lib)
        find_library(_client_lib_found NAMES ${_name}
          PATHS "${_root}/lib64" "${_root}/lib" "${_root}/lib/x86_64-linux-gnu"
          NO_DEFAULT_PATH)
        if(_client_lib_found)
          set(_client_lib "${_client_lib_found}")
        endif()
        unset(_client_lib_found CACHE)
      endif()
    endforeach()
  endif()
endforeach()
if(_client_inc AND _client_lib)
  message(STATUS "accel: MySQL client library for in-process scans: ${_client_lib}")
  set(_client_defs "ACCEL_MYSQL_HAVE_CLIENT=1")
  # MariaDB Connector/C loads its auth plugins (caching_sha2_password, ...)
  # from a directory; point it at the one next to the library we found.
  if(EXISTS "${_client_root}/lib64/mariadb/plugin")
    list(APPEND _client_defs "ACCEL_MYSQL_CLIENT_PLUGIN_DIR=\"${_client_root}/lib64/mariadb/plugin\"")
  endif()
  set_source_files_properties(backends/mysql/api/runtime.cpp PROPERTIES
    COMPILE_DEFINITIONS "${_client_defs}"
    INCLUDE_DIRECTORIES "${_client_inc}")
  # Loaded with dlopen at run time, not linked (see ClientApi in runtime.cpp).
  get_filename_component(_client_lib_real "${_client_lib}" REALPATH)
  list(APPEND _client_defs "ACCEL_MYSQL_CLIENT_LIB_PATH=\"${_client_lib_real}\"")
  set_source_files_properties(backends/mysql/api/runtime.cpp PROPERTIES
    COMPILE_DEFINITIONS "${_client_defs}")
  target_link_libraries(accelerator_codegen_mysql PRIVATE ${CMAKE_DL_LIBS})
else()
  message(STATUS "accel: no MySQL client library found; scans use the mysql CLI (no snapshot builds)")
endif()

# Layer harnesses: every backends/mysql/tests/test_mysql_<layer>.cpp.
file(GLOB _mysql_layer_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/backends/mysql/tests/test_mysql_*.cpp")
list(SORT _mysql_layer_tests)
foreach(_src IN LISTS _mysql_layer_tests)
    get_filename_component(_target "${_src}" NAME_WE)
    string(REGEX REPLACE "^test_mysql_" "" _layer "${_target}")
    accel_add_backend_exe(mysql ${_target} "${_src}")
    if(BUILD_TESTING)
        add_test(NAME engine_mysql_${_layer} COMMAND ${_target})
    endif()
endforeach()

