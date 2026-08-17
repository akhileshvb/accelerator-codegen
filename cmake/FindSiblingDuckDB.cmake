set(_duckdb_root_candidates)

if(DEFINED ENV{ACCEL_DUCKDB_ROOT} AND NOT "$ENV{ACCEL_DUCKDB_ROOT}" STREQUAL "")
    list(APPEND _duckdb_root_candidates "$ENV{ACCEL_DUCKDB_ROOT}")
endif()

# Pack worktrees nest under pipeline/worktrees/<id>; sibling lookup from that
# depth fails. Prefer the primary checkout when the pipeline sets it.
if(DEFINED ENV{ACCEL_PIPELINE_PRIMARY_REPO} AND NOT "$ENV{ACCEL_PIPELINE_PRIMARY_REPO}" STREQUAL "")
    list(APPEND _duckdb_root_candidates "$ENV{ACCEL_PIPELINE_PRIMARY_REPO}/../accelerator-codegen-duckdb")
endif()

list(APPEND _duckdb_root_candidates "${CMAKE_CURRENT_LIST_DIR}/../../accelerator-codegen-duckdb")
# Worktree depth: .../pipeline/worktrees/<worker>/cmake → four levels up to siblings.
list(APPEND _duckdb_root_candidates "${CMAKE_CURRENT_LIST_DIR}/../../../../accelerator-codegen-duckdb")

set(DUCKDB_ROOT "")
foreach(candidate IN LISTS _duckdb_root_candidates)
    get_filename_component(candidate_abs "${candidate}" ABSOLUTE)
    if(EXISTS "${candidate_abs}/src/include/duckdb.hpp")
        set(DUCKDB_ROOT "${candidate_abs}")
        break()
    endif()
endforeach()

if(DUCKDB_ROOT STREQUAL "")
    message(FATAL_ERROR
        "Could not find sibling DuckDB checkout. Set ACCEL_DUCKDB_ROOT or place the checkout at ../accelerator-codegen-duckdb.")
endif()

set(_duckdb_shared_lib "${DUCKDB_ROOT}/build/release/src/libduckdb.so")
set(_duckdb_static_lib "${DUCKDB_ROOT}/build/release/src/libduckdb_static.a")

if(EXISTS "${_duckdb_shared_lib}")
    set(DUCKDB_LIBRARY "${_duckdb_shared_lib}")
elseif(EXISTS "${_duckdb_static_lib}")
    set(DUCKDB_LIBRARY "${_duckdb_static_lib}")
else()
    message(FATAL_ERROR
        "DuckDB library not found under ${DUCKDB_ROOT}/build/release/src. Build the sibling DuckDB checkout first.")
endif()

get_filename_component(DUCKDB_LIBRARY_DIR "${DUCKDB_LIBRARY}" DIRECTORY)

set(DUCKDB_INCLUDE_DIR "${DUCKDB_ROOT}/src/include")

if(NOT EXISTS "${DUCKDB_INCLUDE_DIR}/duckdb.hpp")
    message(FATAL_ERROR "DuckDB headers not found under ${DUCKDB_INCLUDE_DIR}.")
endif()

add_library(duckdb_sibling UNKNOWN IMPORTED)
set_target_properties(duckdb_sibling PROPERTIES
    IMPORTED_LOCATION "${DUCKDB_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${DUCKDB_INCLUDE_DIR}"
)

message(STATUS "Using DuckDB checkout: ${DUCKDB_ROOT}")
message(STATUS "Using DuckDB library: ${DUCKDB_LIBRARY}")
