cmake_minimum_required(VERSION 3.20)

project(accelerator_codegen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ACCEL_AUX_CALIBRATION
       "Build auxiliary cost-model calibration benchmarks" OFF)
option(ACCEL_CALIBRATE_AT_BUILD
       "Run shared aux cost-model calibration during the default build (requires DBs)" OFF)

# ACCEL_SANITIZE=thread|address instruments our targets (address: with UBSan); the sibling DuckDB stays uninstrumented.
# Instrumented builds compile at -O1 with line tables: half the compile time of -O2 -g, and the reports keep file:line.
set(ACCEL_SANITIZE "" CACHE STRING "Sanitizer for our targets (thread, address)")
if(ACCEL_SANITIZE)
    add_compile_options(-O1 -g1)
endif()
if(ACCEL_SANITIZE STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
elseif(ACCEL_SANITIZE STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
elseif(ACCEL_SANITIZE)
    message(FATAL_ERROR "ACCEL_SANITIZE must be empty, 'thread' or 'address'")
endif()

include(CTest)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(FindSiblingDuckDB)
include(AuxState)
include(BaseQuery)
include(GeneratedSources)
include(Backend)

# TPC-H query instances (workloads/tpch/instances/seed_N/NN.sql) read by the accelerator tests and benchmarks.
add_compile_definitions(ACCEL_TPCH_INSTANCES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/workloads/tpch/instances")

accel_collect_stateful_sources(ACCEL_STATEFUL_SOURCES)

add_library(accelerator_codegen SHARED ${ACCEL_STATEFUL_SOURCES})

target_include_directories(accelerator_codegen
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators
)

# Generated query code includes DuckDB headers; the substrate itself does not.
if(TARGET duckdb_sibling)
    target_link_libraries(accelerator_codegen PUBLIC duckdb_sibling)
    set_target_properties(accelerator_codegen PROPERTIES
        BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
        INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}")
endif()

target_compile_features(accelerator_codegen PUBLIC cxx_std_17)
accel_apply_aux_copts(substrate/aux/state/versioned_aux_store.cpp)
include(VerusKits)

# Engines: each backends/<engine>/backend.cmake (see cmake/Backend.cmake).
# Generated trees: accelerators/<bench>/build.cmake, and the aux state of every query.
# Fresh per-workload test databases (built by the <name>_test_db fixtures below); set
# before the backends so their harnesses can name them.
set(ACCEL_TEST_DB_DIR "${CMAKE_CURRENT_BINARY_DIR}/test_dbs")
accel_include_backends()
accel_include_accelerators()
include(AgentExecutables)
accel_add_agent_executables()
accel_register_aux_state_queries(accelerator_codegen)

# Engine-neutral unit tests of the substrate and the test-support headers.
function(accel_add_unit_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE accelerator_codegen)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)
    if(BUILD_TESTING)
        add_test(NAME ${ARGN} COMMAND ${target})
    endif()
endfunction()

accel_add_unit_test(test_aux_runtime_units eval/tests/test_aux_runtime_units.cpp aux_runtime_units_test)
accel_add_unit_test(test_aux_runtime eval/tests/test_aux_runtime.cpp aux_runtime_test)
accel_add_unit_test(test_aux_wire eval/tests/test_aux_wire.cpp aux_wire_test)
add_executable(tsan_module_driver eval/tests/tsan_module_driver.cpp)
target_link_libraries(tsan_module_driver PRIVATE accelerator_codegen)
target_include_directories(tsan_module_driver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)
accel_add_unit_test(test_core_helpers eval/tests/test_core_helpers.cpp core_helpers_test)
accel_add_unit_test(test_versioned_aux_store_caps eval/tests/test_versioned_aux_store_caps.cpp versioned_aux_store_caps_test)
accel_add_unit_test(test_scratch_dir eval/tests/test_scratch_dir.cpp scratch_dir_cleanup_test)
accel_add_unit_test(test_float_order_ties eval/tests/test_float_order_ties.cpp test_float_order_ties)

# Generic primitives library (accelerators/prims/, see its README).
# Header-only; no DBMS needed. New primitives add their tests to test_prims.cpp.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/accelerators/prims/tests/test_prims.cpp")
    find_package(Threads REQUIRED)
    add_executable(test_prims accelerators/prims/tests/test_prims.cpp)
    target_include_directories(test_prims PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)
    target_link_libraries(test_prims PRIVATE Threads::Threads)
    if(BUILD_TESTING)
        add_test(NAME prims_unit_test COMMAND test_prims)
    endif()
endif()

# Test databases: for every workloads/<name>/test_db_subset.sql, the fixture
# <name>_test_db builds a fresh small DuckDB file under ACCEL_TEST_DB_DIR from the
# workload's source DB, so tests never depend on a shared database that may be stale
# or modified by other runs. Exits 77 (skip) when the source is missing or empty;
# tests needing the DB then skip too.
if(TARGET duckdb_sibling)
    file(GLOB _accel_test_db_subsets CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/workloads/*/test_db_subset.sql")
    foreach(_subset IN LISTS _accel_test_db_subsets)
        get_filename_component(_dir "${_subset}" DIRECTORY)
        get_filename_component(_workload "${_dir}" NAME)
        set(_maker eval/tests/make_${_workload}_test_db.cpp)
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_maker}")
            continue()
        endif()
        add_executable(make_${_workload}_test_db ${_maker})
        target_link_libraries(make_${_workload}_test_db PRIVATE accelerator_codegen duckdb_sibling)
        target_include_directories(make_${_workload}_test_db PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)
        if(BUILD_TESTING)
            # Builds the maker first: a build that only produced the backend's own targets still has it.
            add_test(NAME ${_workload}_test_db_maker
                     COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target make_${_workload}_test_db)
            set_tests_properties(${_workload}_test_db_maker PROPERTIES FIXTURES_SETUP ${_workload}_test_db_maker)
            add_test(NAME ${_workload}_test_db_fixture
                     COMMAND make_${_workload}_test_db "${_subset}" "${ACCEL_TEST_DB_DIR}/${_workload}_test.duckdb")
            set_tests_properties(${_workload}_test_db_fixture PROPERTIES
                FIXTURES_SETUP ${_workload}_test_db FIXTURES_REQUIRED ${_workload}_test_db_maker
                SKIP_RETURN_CODE 77)
        endif()
    endforeach()
endif()
