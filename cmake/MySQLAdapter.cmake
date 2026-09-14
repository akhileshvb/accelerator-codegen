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
endfunction()
