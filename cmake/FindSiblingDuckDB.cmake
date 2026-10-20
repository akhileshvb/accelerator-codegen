# The sibling DuckDB is an optional third-party dependency: the substrate library does
# not include or link it. Found, it becomes the imported target duckdb_sibling (and the
# library links it, since generated query code includes its headers); absent, the build
# carries on without it and accel_require_duckdb() is how a backend or tool that
# needs it fails with the reason.
#
# The DuckDB fork the DuckDB backend needs is the capture fork (branch skeleton/db-capture,
# checked out at ../duckdb-dbcapture): it adds DatabaseCaptureRegistry,
# CommitIds::OnCommitWithContext and ActiveWriterStarts(), which the DuckDB
# backend uses. A stock or older fork fails to compile. Set ACCEL_DUCKDB_ROOT to
# build against a checkout elsewhere.
set(_duckdb_root_candidates)

if(DEFINED ENV{ACCEL_DUCKDB_ROOT} AND NOT "$ENV{ACCEL_DUCKDB_ROOT}" STREQUAL "")
    list(APPEND _duckdb_root_candidates "$ENV{ACCEL_DUCKDB_ROOT}")
endif()

# Pack worktrees nest under pipeline/worktrees/<id>; sibling lookup from that
# depth fails. Prefer the primary checkout when the pipeline sets it.
if(DEFINED ENV{ACCEL_PIPELINE_PRIMARY_REPO} AND NOT "$ENV{ACCEL_PIPELINE_PRIMARY_REPO}" STREQUAL "")
    list(APPEND _duckdb_root_candidates "$ENV{ACCEL_PIPELINE_PRIMARY_REPO}/../duckdb-dbcapture")
endif()

list(APPEND _duckdb_root_candidates "${CMAKE_CURRENT_LIST_DIR}/../../duckdb-dbcapture")
# Worktree depth: .../pipeline/worktrees/<worker>/cmake → four levels up to siblings.
list(APPEND _duckdb_root_candidates "${CMAKE_CURRENT_LIST_DIR}/../../../../duckdb-dbcapture")

set(DUCKDB_ROOT "")
foreach(candidate IN LISTS _duckdb_root_candidates)
    get_filename_component(candidate_abs "${candidate}" ABSOLUTE)
    if(EXISTS "${candidate_abs}/src/include/duckdb.hpp")
        set(DUCKDB_ROOT "${candidate_abs}")
        break()
    endif()
endforeach()

set(ACCEL_DUCKDB_MISSING "")
if(DUCKDB_ROOT STREQUAL "")
    set(ACCEL_DUCKDB_MISSING
        "Could not find sibling DuckDB checkout. Set ACCEL_DUCKDB_ROOT or place the capture fork at ../duckdb-dbcapture.")
else()
    set(_duckdb_shared_lib "${DUCKDB_ROOT}/build/release/src/libduckdb.so")
    set(_duckdb_static_lib "${DUCKDB_ROOT}/build/release/src/libduckdb_static.a")
    if(EXISTS "${_duckdb_shared_lib}")
        set(DUCKDB_LIBRARY "${_duckdb_shared_lib}")
    elseif(EXISTS "${_duckdb_static_lib}")
        set(DUCKDB_LIBRARY "${_duckdb_static_lib}")
    else()
        set(ACCEL_DUCKDB_MISSING
            "DuckDB library not found under ${DUCKDB_ROOT}/build/release/src. Build the sibling DuckDB checkout first.")
    endif()
endif()

if(ACCEL_DUCKDB_MISSING STREQUAL "")
    get_filename_component(DUCKDB_LIBRARY_DIR "${DUCKDB_LIBRARY}" DIRECTORY)
    set(DUCKDB_INCLUDE_DIR "${DUCKDB_ROOT}/src/include")
    add_library(duckdb_sibling UNKNOWN IMPORTED)
    set_target_properties(duckdb_sibling PROPERTIES
        IMPORTED_LOCATION "${DUCKDB_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${DUCKDB_INCLUDE_DIR}"
    )
    message(STATUS "Using DuckDB checkout: ${DUCKDB_ROOT}")
    message(STATUS "Using DuckDB library: ${DUCKDB_LIBRARY}")
else()
    message(STATUS "DuckDB not available (${ACCEL_DUCKDB_MISSING})")
endif()

macro(accel_require_duckdb)
    if(NOT TARGET duckdb_sibling)
        message(FATAL_ERROR "${ACCEL_DUCKDB_MISSING}")
    endif()
endmacro()
