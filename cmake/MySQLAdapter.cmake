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

  # Verus-verified stateless queries (e.g. scripts/build_q1_verus.sh builds
  # build-verus/libq1_verus.a): src/generated/<bench>/q<N>/mysql/verus/entry.cpp
  # is linked when build-verus/libq<N>_verus.a exists.
  set(_mysql_verus_libs "")
  file(GLOB _mysql_verus_entries CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/*/q*/mysql/verus/entry.cpp")
  list(SORT _mysql_verus_entries)
  foreach(_entry IN LISTS _mysql_verus_entries)
    get_filename_component(_verus_dir "${_entry}" DIRECTORY)
    get_filename_component(_verus_dir "${_verus_dir}" DIRECTORY)
    get_filename_component(_verus_q "${_verus_dir}" NAME)
    set(_verus_lib "${CMAKE_CURRENT_SOURCE_DIR}/build-verus/lib${_verus_q}_verus.a")
    if(EXISTS "${_verus_lib}")
      list(APPEND _mysql_plugin_sources "${_entry}")
      list(APPEND _mysql_verus_libs "${_verus_lib}")
    endif()
  endforeach()

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
  if(_mysql_verus_libs)
    target_link_libraries(accelerator_codegen_mysql PRIVATE ${_mysql_verus_libs} pthread dl m)
  endif()
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
endfunction()
