# Consume orchestrator-resolved PostgreSQL *server* SDK (no independent search).
#
# The accel_pipeline orchestrator discovers a complete install (pacman/apt/
# Homebrew/$HOME/opt/…) and exports PG_CONFIG / ACCEL_PG_* into the process
# environment before cmake/gates run. This module only validates that hand-off.
#
# Required (any one):
#   -D PG_CONFIG=/path/to/pg_config
#   env PG_CONFIG / ACCEL_PG_CONFIG
#   env ACCEL_PG_BINDIR  (…/bin) or ACCEL_PG_ROOT (prefix)
#
# Completeness check: includedir-server/postgres.h + bindir/{initdb,postgres}.
#
# Usage (when ACCELERATOR_CODEGEN_BUILD_POSTGRES=ON):
#   include(FindAccelPostgreSQL)   # sets ACCEL_PG_* or leaves ACCEL_PG_FOUND=FALSE

function(_accel_pg_is_complete PG_CONFIG_ABS OUT_VAR)
    set(${OUT_VAR} FALSE PARENT_SCOPE)
    if(NOT EXISTS "${PG_CONFIG_ABS}")
        return()
    endif()
    execute_process(
        COMMAND "${PG_CONFIG_ABS}" --includedir-server
        OUTPUT_VARIABLE _inc_server
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _rc1
        ERROR_QUIET
    )
    execute_process(
        COMMAND "${PG_CONFIG_ABS}" --bindir
        OUTPUT_VARIABLE _bindir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _rc2
        ERROR_QUIET
    )
    if(NOT _rc1 EQUAL 0 OR NOT _rc2 EQUAL 0)
        return()
    endif()
    if(NOT EXISTS "${_inc_server}/postgres.h")
        return()
    endif()
    if(NOT EXISTS "${_bindir}/initdb" OR NOT EXISTS "${_bindir}/postgres")
        return()
    endif()
    set(${OUT_VAR} TRUE PARENT_SCOPE)
endfunction()

function(_accel_pg_use_config CANDIDATE)
    if(ACCEL_PG_FOUND OR NOT CANDIDATE)
        return()
    endif()
    get_filename_component(_abs "${CANDIDATE}" ABSOLUTE)
    _accel_pg_is_complete("${_abs}" _ok)
    if(NOT _ok)
        message(STATUS
            "accel: ignoring incomplete PG_CONFIG=${_abs} "
            "(need postgres.h + initdb/postgres; client-only installs are rejected)")
        return()
    endif()

    execute_process(COMMAND "${_abs}" --includedir-server
        OUTPUT_VARIABLE ACCEL_PG_INCLUDEDIR_SERVER OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND "${_abs}" --includedir
        OUTPUT_VARIABLE ACCEL_PG_INCLUDEDIR OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND "${_abs}" --bindir
        OUTPUT_VARIABLE ACCEL_PG_BINDIR OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND "${_abs}" --libdir
        OUTPUT_VARIABLE ACCEL_PG_LIBDIR OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND "${_abs}" --version
        OUTPUT_VARIABLE ACCEL_PG_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)

    set(ACCEL_PG_FOUND TRUE PARENT_SCOPE)
    set(ACCEL_PG_CONFIG "${_abs}" PARENT_SCOPE)
    set(ACCEL_PG_INCLUDEDIR_SERVER "${ACCEL_PG_INCLUDEDIR_SERVER}" PARENT_SCOPE)
    set(ACCEL_PG_INCLUDEDIR "${ACCEL_PG_INCLUDEDIR}" PARENT_SCOPE)
    set(ACCEL_PG_BINDIR "${ACCEL_PG_BINDIR}" PARENT_SCOPE)
    set(ACCEL_PG_LIBDIR "${ACCEL_PG_LIBDIR}" PARENT_SCOPE)
    set(ACCEL_PG_VERSION "${ACCEL_PG_VERSION}" PARENT_SCOPE)
endfunction()

set(ACCEL_PG_FOUND FALSE)

# Cache -D PG_CONFIG=… (preferred when gates pass it explicitly)
if(PG_CONFIG)
    _accel_pg_use_config("${PG_CONFIG}")
endif()
if(NOT ACCEL_PG_FOUND AND DEFINED ENV{PG_CONFIG} AND NOT "$ENV{PG_CONFIG}" STREQUAL "")
    _accel_pg_use_config("$ENV{PG_CONFIG}")
endif()
if(NOT ACCEL_PG_FOUND AND DEFINED ENV{ACCEL_PG_CONFIG} AND NOT "$ENV{ACCEL_PG_CONFIG}" STREQUAL "")
    _accel_pg_use_config("$ENV{ACCEL_PG_CONFIG}")
endif()
if(NOT ACCEL_PG_FOUND AND DEFINED ENV{ACCEL_PG_BINDIR} AND NOT "$ENV{ACCEL_PG_BINDIR}" STREQUAL "")
    _accel_pg_use_config("$ENV{ACCEL_PG_BINDIR}/pg_config")
endif()
if(NOT ACCEL_PG_FOUND AND DEFINED ENV{ACCEL_PG_ROOT} AND NOT "$ENV{ACCEL_PG_ROOT}" STREQUAL "")
    _accel_pg_use_config("$ENV{ACCEL_PG_ROOT}/bin/pg_config")
endif()

if(ACCEL_PG_FOUND)
    get_filename_component(ACCEL_PG_ROOT "${ACCEL_PG_BINDIR}" DIRECTORY)
    message(STATUS "accel: PostgreSQL ${ACCEL_PG_VERSION} (orchestrator-resolved)")
    message(STATUS "accel:   pg_config=${ACCEL_PG_CONFIG}")
    message(STATUS "accel:   bindir=${ACCEL_PG_BINDIR}")
    message(STATUS "accel:   server_include=${ACCEL_PG_INCLUDEDIR_SERVER}")
else()
    message(STATUS
        "accel: no orchestrator-resolved PostgreSQL SDK "
        "(set PG_CONFIG / ACCEL_PG_* before cmake; do not rely on PATH search here)")
endif()
