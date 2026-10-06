# MySQL engine adapter — build the plugin (accelerator_codegen_mysql) and
# engine_backend layer harnesses inside accel_mysql_test Docker container.
#
# This file grows with each engine_backend layer. Early layers only need
# the shared library; later layers add plugin sources here.
function(accel_build_mysql_plugin)
  set(_mysql_plugin_sources "")

  foreach(_rel
      src/runtime/mysql/mysql_plugin.cpp
      src/generated/mysql/mysql_plugin.cpp
      src/generated/mysql/runtime.cpp
      src/stateful/mysql/mysql_bridge.cpp
      src/stateful/mysql/mysql_xact_callbacks.cpp
  )
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_rel}")
      list(APPEND _mysql_plugin_sources "${_rel}")
    endif()
  endforeach()

  if(NOT _mysql_plugin_sources)
    return()
  endif()

  # Per-query plugin code (scans, runtimes, UDF shims): every query registers
  # itself through the backend registries (src/generated/mysql/runtime.hpp), so
  # the backend builds with or without any of these directories.
  file(GLOB _mysql_query_plugin_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/*/q*/mysql/plugin/*.cpp")
  list(SORT _mysql_query_plugin_sources)
  list(APPEND _mysql_plugin_sources ${_mysql_query_plugin_sources})

  add_library(accelerator_codegen_mysql MODULE ${_mysql_plugin_sources})
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
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
  )
  if(ACCEL_MYSQL_FOUND AND ACCEL_MYSQL_INCLUDEDIR)
    target_include_directories(accelerator_codegen_mysql
      PRIVATE
        ${ACCEL_MYSQL_INCLUDEDIR}
    )
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
    set_source_files_properties(src/generated/mysql/runtime.cpp PROPERTIES
      COMPILE_DEFINITIONS "${_client_defs}"
      INCLUDE_DIRECTORIES "${_client_inc}")
    # Loaded with dlopen at run time, not linked (see ClientApi in runtime.cpp).
    get_filename_component(_client_lib_real "${_client_lib}" REALPATH)
    list(APPEND _client_defs "ACCEL_MYSQL_CLIENT_LIB_PATH=\"${_client_lib_real}\"")
    set_source_files_properties(src/generated/mysql/runtime.cpp PROPERTIES
      COMPILE_DEFINITIONS "${_client_defs}")
    target_link_libraries(accelerator_codegen_mysql PRIVATE ${CMAKE_DL_LIBS})
  else()
    message(STATUS "accel: no MySQL client library found; scans use the mysql CLI (no snapshot builds)")
  endif()
endfunction()
