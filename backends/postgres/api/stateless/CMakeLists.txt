# Stateless PostgreSQL accelerators: a self-contained PostgreSQL module
# (no DuckDB, no aux-state stack).  Built inside the PostgreSQL container by
# scripts/build_stateless_pg.sh:
#
#   cmake -S <root>/src/runtime/postgres_stateless -B <build> -G Ninja \
#         -DACCEL_ROOT=<root> -DACCEL_STATELESS_PG_LIB=accel_stateless_pg
cmake_minimum_required(VERSION 3.16)
project(accel_stateless_pg CXX)

set(ACCEL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." CACHE PATH "repository root")
set(ACCEL_STATELESS_PG_LIB "accel_stateless_pg" CACHE STRING "module name ($libdir/<name>.so)")
set(ACCEL_STATELESS_COPTS "-march=native" CACHE STRING "extra compile options (SIMD paths need the target ISA)")
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_program(PG_CONFIG pg_config REQUIRED)
execute_process(COMMAND ${PG_CONFIG} --includedir-server OUTPUT_VARIABLE PG_INCLUDEDIR_SERVER
                OUTPUT_STRIP_TRAILING_WHITESPACE)

# ACCEL_STATELESS_QUERIES: optional list of TPC-H query numbers to build (default: all).
set(ACCEL_STATELESS_QUERIES "" CACHE STRING "TPC-H queries to include (e.g. 1;6;14), empty = all")
if(ACCEL_STATELESS_QUERIES)
    set(_kernel_sources "")
    foreach(_q IN LISTS ACCEL_STATELESS_QUERIES)
        file(GLOB _qs CONFIGURE_DEPENDS
            "${ACCEL_ROOT}/accelerators/tpch/q${_q}/postgres/stateless/*.cpp"
            "${ACCEL_ROOT}/accelerators/tpch/q${_q}/postgres/stateless/*/*.cpp")
        list(APPEND _kernel_sources ${_qs})
    endforeach()
else()
    file(GLOB _kernel_sources CONFIGURE_DEPENDS
        "${ACCEL_ROOT}/accelerators/*/q*/postgres/stateless/*.cpp"
        "${ACCEL_ROOT}/accelerators/*/q*/postgres/stateless/*/*.cpp")
endif()
list(SORT _kernel_sources)

add_library(${ACCEL_STATELESS_PG_LIB} MODULE
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime_common.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime_pg.cpp
    ${_kernel_sources})
set_target_properties(${ACCEL_STATELESS_PG_LIB} PROPERTIES PREFIX "" SUFFIX ".so"
                      CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
                      CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(${ACCEL_STATELESS_PG_LIB} PRIVATE
    ${ACCEL_ROOT} ${ACCEL_ROOT}/accelerators ${ACCEL_ROOT} ${ACCEL_ROOT}/accelerators ${CMAKE_CURRENT_SOURCE_DIR} ${PG_INCLUDEDIR_SERVER})
target_compile_definitions(${ACCEL_STATELESS_PG_LIB} PRIVATE
    ACCEL_STATELESS_PG_LIB="${ACCEL_STATELESS_PG_LIB}")
separate_arguments(_copts UNIX_COMMAND "${ACCEL_STATELESS_COPTS}")
target_compile_options(${ACCEL_STATELESS_PG_LIB} PRIVATE ${_copts} -Wall -Wno-unused-function -Wno-sign-compare)
find_package(Threads REQUIRED)
target_link_libraries(${ACCEL_STATELESS_PG_LIB} PRIVATE Threads::Threads)
