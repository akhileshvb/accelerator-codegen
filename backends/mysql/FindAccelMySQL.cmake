# Consume orchestrator / in-container MySQL *plugin* SDK (not client-only mysql_config).
#
# Completeness: mysql/plugin.h (or mysql/mysql/plugin.h). Optional: mysql_config.
#
# Usage (when ACCELERATOR_CODEGEN_BUILD_MYSQL=ON):
#   include(FindAccelMySQL)   # sets ACCEL_MYSQL_* or leaves ACCEL_MYSQL_FOUND=FALSE

set(ACCEL_MYSQL_FOUND FALSE)
set(ACCEL_MYSQL_INCLUDEDIR "")
set(ACCEL_MYSQL_PLUGIN_HEADER "")

set(_accel_mysql_inc_candidates "")
if(MYSQL_INCLUDE_DIR)
    list(APPEND _accel_mysql_inc_candidates "${MYSQL_INCLUDE_DIR}")
endif()
if(DEFINED ENV{ACCEL_MYSQL_INCLUDEDIR} AND NOT "$ENV{ACCEL_MYSQL_INCLUDEDIR}" STREQUAL "")
    list(APPEND _accel_mysql_inc_candidates "$ENV{ACCEL_MYSQL_INCLUDEDIR}")
endif()
list(APPEND _accel_mysql_inc_candidates
    /usr/include/mysql
    /usr/include/mysql/mysql
    /usr/local/include/mysql
    /usr/include
)

foreach(_inc IN LISTS _accel_mysql_inc_candidates)
    if(EXISTS "${_inc}/mysql/plugin.h")
        set(ACCEL_MYSQL_FOUND TRUE)
        set(ACCEL_MYSQL_INCLUDEDIR "${_inc}")
        set(ACCEL_MYSQL_PLUGIN_HEADER "${_inc}/mysql/plugin.h")
        break()
    endif()
    if(EXISTS "${_inc}/plugin.h")
        set(ACCEL_MYSQL_FOUND TRUE)
        get_filename_component(_parent "${_inc}" DIRECTORY)
        set(ACCEL_MYSQL_INCLUDEDIR "${_parent}")
        set(ACCEL_MYSQL_PLUGIN_HEADER "${_inc}/plugin.h")
        break()
    endif()
endforeach()

if(ACCEL_MYSQL_FOUND)
    message(STATUS "accel: MySQL plugin headers (orchestrator / in-container)")
    message(STATUS "accel:   include=${ACCEL_MYSQL_INCLUDEDIR}")
    message(STATUS "accel:   plugin.h=${ACCEL_MYSQL_PLUGIN_HEADER}")
else()
    message(STATUS
        "accel: no MySQL plugin headers yet "
        "(set MYSQL_INCLUDE_DIR / ACCEL_MYSQL_INCLUDEDIR; "
        "from-scratch engine_backend may still configure)")
endif()
