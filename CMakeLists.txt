cmake_minimum_required(VERSION 3.20)

project(accelerator_codegen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ACCEL_AUX_CALIBRATION
       "Build auxiliary cost-model calibration benchmarks" OFF)
option(ACCEL_CALIBRATE_AT_BUILD
       "Run shared aux cost-model calibration during the default build (requires DBs)" OFF)

include(CTest)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(FindSiblingDuckDB)
include(AuxState)
include(BaseQuery)
include(GeneratedSources)

# Optional Postgres engine adapter. Discovery rejects client-only pg_config.
option(ACCELERATOR_CODEGEN_BUILD_POSTGRES
       "Build the optional PostgreSQL engine adapter (requires server SDK)" OFF)
if(ACCELERATOR_CODEGEN_BUILD_POSTGRES)
	include(FindAccelPostgreSQL)
	if(NOT ACCEL_PG_FOUND)
		# Host cmake in Docker pipeline mode must not FATAL — PG MODULE builds
		# inside accel_pg_test. In-container configures always pass PG_CONFIG.
		if(DEFINED ENV{ACCEL_PG_DOCKER} AND "$ENV{ACCEL_PG_DOCKER}" STREQUAL "1")
			message(STATUS
				"accel: BUILD_POSTGRES ON but no host SDK (ACCEL_PG_DOCKER=1); "
				"skipping host MODULE — build inside Docker instead")
		else()
			message(FATAL_ERROR
				"ACCELERATOR_CODEGEN_BUILD_POSTGRES=ON but no orchestrator-resolved "
				"PostgreSQL server SDK was provided. For the Docker pipeline use "
				"scripts/ensure_accel_pg_test.sh (in-container build). "
				"Or export PG_CONFIG to a complete install (postgres.h + initdb).")
		endif()
	endif()
	if(ACCEL_PG_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PostgresAdapter.cmake")
		include(PostgresAdapter)
		accel_build_postgres_extension()
	elseif(ACCEL_PG_FOUND)
		message(STATUS
			"accel: BUILD_POSTGRES ON; cmake/PostgresAdapter.cmake not present yet "
			"(implementation stage). Server SDK OK: ${ACCEL_PG_CONFIG}")
	endif()
endif()

# Optional MySQL engine adapter. Plugin headers are discovered in-container.
option(ACCELERATOR_CODEGEN_BUILD_MYSQL
       "Build the optional MySQL engine adapter (requires plugin SDK)" OFF)
if(ACCELERATOR_CODEGEN_BUILD_MYSQL)
	if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindAccelMySQL.cmake")
		include(FindAccelMySQL)
	endif()
	if(NOT ACCEL_MYSQL_FOUND)
		if(DEFINED ENV{ACCEL_MYSQL_DOCKER} AND "$ENV{ACCEL_MYSQL_DOCKER}" STREQUAL "1")
			message(STATUS
				"accel: BUILD_MYSQL ON but no plugin headers yet (ACCEL_MYSQL_DOCKER=1); "
				"store/aux layers may still build; plugin TUs wait for mysql/plugin.h")
		else()
			message(STATUS
				"accel: BUILD_MYSQL ON without mysql/plugin.h — "
				"standalone harnesses can still link accelerator_codegen")
		endif()
	endif()
	if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MySQLAdapter.cmake")
		include(MySQLAdapter)
		accel_build_mysql_plugin()
	endif()
endif()

accel_collect_runtime_sources(ACCEL_RUNTIME_SOURCES)
accel_collect_stateful_sources(ACCEL_STATEFUL_SOURCES)
accel_collect_generated_sources(ACCEL_GENERATED_SOURCES)

add_library(accelerator_codegen SHARED
    ${ACCEL_RUNTIME_SOURCES}
    ${ACCEL_STATEFUL_SOURCES}
    ${ACCEL_GENERATED_SOURCES}
)

target_include_directories(accelerator_codegen
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
)
# Expose per-engine catalog as "catalog/query_catalog.hpp" via a build-tree prefix.
accel_codegen_engine_name(_ACCEL_CATALOG_ENGINE)
if(_ACCEL_CATALOG_ENGINE STREQUAL "")
	if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/catalog/duckdb/query_catalog.hpp")
		set(_ACCEL_CATALOG_ENGINE "duckdb")
	endif()
endif()
if(NOT _ACCEL_CATALOG_ENGINE STREQUAL "")
	set(_ACCEL_CATALOG_INCLUDE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/generated_includes")
	file(MAKE_DIRECTORY "${_ACCEL_CATALOG_INCLUDE_ROOT}")
	set(_ACCEL_CATALOG_LINK "${_ACCEL_CATALOG_INCLUDE_ROOT}/catalog")
	set(_ACCEL_CATALOG_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/catalog/${_ACCEL_CATALOG_ENGINE}")
	if(EXISTS "${_ACCEL_CATALOG_SRC}/query_catalog.hpp")
		if(NOT EXISTS "${_ACCEL_CATALOG_LINK}")
			execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink
				"${_ACCEL_CATALOG_SRC}" "${_ACCEL_CATALOG_LINK}")
		endif()
		target_include_directories(accelerator_codegen
			PUBLIC
				${_ACCEL_CATALOG_INCLUDE_ROOT}
		)
	endif()
endif()
if(ACCELERATOR_CODEGEN_BUILD_POSTGRES AND ACCEL_PG_FOUND)
    target_include_directories(accelerator_codegen
        PRIVATE
            ${ACCEL_PG_INCLUDEDIR}
            ${ACCEL_PG_INCLUDEDIR_SERVER}
    )
endif()


target_link_libraries(accelerator_codegen
    PUBLIC
        duckdb_sibling
)

# Verus-verified Q21 kernel (scripts/build_q21_verus.sh builds the archive).
# The engine-neutral module and the kernel live in accelerator_codegen so that
# every engine adapter (DuckDB runtime, Postgres extension, MySQL plugin)
# links exactly one copy.
set(ACCEL_Q21_VERUS_LIB "${CMAKE_CURRENT_SOURCE_DIR}/build-verus/libq21_verus.a"
    CACHE FILEPATH "Verus Q21 kernel static library")
if(EXISTS "${ACCEL_Q21_VERUS_LIB}")
    message(STATUS "Q21 Verus kernel: ${ACCEL_Q21_VERUS_LIB}")
    target_sources(accelerator_codegen PRIVATE src/generated/tpch/q21/verus/module.cpp)
    target_compile_definitions(accelerator_codegen PUBLIC ACCEL_HAVE_Q21_VERUS)
    target_link_libraries(accelerator_codegen PRIVATE ${ACCEL_Q21_VERUS_LIB} pthread dl m)
endif()

target_compile_features(accelerator_codegen PUBLIC cxx_std_17)

# Link finalize.cpp for every TPC-H aux module that exists on disk.
accel_register_tpch_aux_finalize_queries(
    accelerator_codegen
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

# Register test/bench/calibrate targets for every test_tpch_q*_aux.cpp present.
accel_register_tpch_aux_pipeline_targets()

# Parallel kernels depend on the DuckDB host adapter headers. When that host is
# quarantined (engine_backend from-scratch), skip even if cache has these ON.
set(_ACCEL_DUCKDB_HOST_PRESENT FALSE)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/duckdb/duckdb_scan_adapter.cpp")
	set(_ACCEL_DUCKDB_HOST_PRESENT TRUE)
endif()

option(ACCEL_ENABLE_PARALLEL_TPCH "Build multithreaded TPC-H accelerator variants (accel_tpch_q*_parallel)" OFF)
if(ACCEL_ENABLE_PARALLEL_TPCH)
	if(NOT _ACCEL_DUCKDB_HOST_PRESENT)
		message(STATUS "accel: DuckDB host absent — skipping ACCEL_ENABLE_PARALLEL_TPCH sources")
	else()
		target_compile_definitions(accelerator_codegen PUBLIC ACCEL_ENABLE_PARALLEL_TPCH)
		# Q3 shared __int128 revenue CAS uses libatomic (__atomic_*_16).
		target_link_libraries(accelerator_codegen PRIVATE atomic)
		accel_collect_generated_parallel_sources(ACCEL_GENERATED_PARALLEL_SOURCES)
		target_sources(accelerator_codegen PRIVATE
			src/runtime/parallel/parallel_runtime.cpp
			${ACCEL_GENERATED_PARALLEL_SOURCES}
		)
	endif()
endif()

option(ACCEL_ENABLE_PARALLEL_AUX "Build multithreaded SEC EDGAR aux kernels (accel_sec_edgar_q*_aux_parallel)" OFF)
if(ACCEL_ENABLE_PARALLEL_AUX)
	if(NOT _ACCEL_DUCKDB_HOST_PRESENT)
		message(STATUS "accel: DuckDB host absent — skipping ACCEL_ENABLE_PARALLEL_AUX sources")
	else()
		target_compile_definitions(accelerator_codegen PUBLIC ACCEL_ENABLE_PARALLEL_AUX)
		accel_collect_generated_parallel_aux_sources(ACCEL_GENERATED_PARALLEL_AUX_SOURCES)
		target_sources(accelerator_codegen PRIVATE
			src/runtime/parallel/aux_parallel_scan.cpp
			${ACCEL_GENERATED_PARALLEL_AUX_SOURCES}
		)
		if(NOT ACCEL_ENABLE_PARALLEL_TPCH)
			target_sources(accelerator_codegen PRIVATE
				src/runtime/parallel/parallel_runtime.cpp
			)
		endif()
	endif()
endif()

add_executable(test_registration
    tests/single-threaded/test_registration.cpp
)

target_link_libraries(test_registration
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

add_executable(test_tpch_q6
    tests/single-threaded/test_tpch_q6.cpp
)

target_link_libraries(test_tpch_q6
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q6 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q1
    tests/single-threaded/test_tpch_q1.cpp
)

target_link_libraries(test_tpch_q1
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q3
    tests/single-threaded/test_tpch_q3.cpp
)

target_link_libraries(test_tpch_q3
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q4
    tests/single-threaded/test_tpch_q4.cpp
)

target_link_libraries(test_tpch_q4
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q4 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q9
    tests/single-threaded/test_tpch_q9.cpp
)

target_link_libraries(test_tpch_q9
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q9 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q12
    tests/single-threaded/test_tpch_q12.cpp
)

target_link_libraries(test_tpch_q12
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q12 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q14
    tests/single-threaded/test_tpch_q14.cpp
)

target_link_libraries(test_tpch_q14
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q14 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_shared_aux_state
    tests/aux_state/test_shared_aux_state.cpp
)

target_link_libraries(test_shared_aux_state
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_shared_aux_state PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

function(accel_add_shared_interface_test target_name test_source)
    add_executable(${target_name} ${test_source})
    target_link_libraries(${target_name} PRIVATE accelerator_codegen duckdb_sibling)
    target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    if(BUILD_TESTING)
        add_test(NAME ${target_name}_unit_test COMMAND ${target_name})
    endif()
endfunction()

accel_add_shared_interface_test(test_shared_tpch_q1 tests/aux_state/test_shared_tpch_q1.cpp)
accel_add_shared_interface_test(test_shared_tpch_q3 tests/aux_state/test_shared_tpch_q3.cpp)
accel_add_shared_interface_test(test_shared_tpch_q6 tests/aux_state/test_shared_tpch_q6.cpp)
accel_add_shared_interface_test(test_shared_tpch_q10 tests/aux_state/test_shared_q10.cpp)
accel_add_shared_interface_test(test_shared_tpch_q12 tests/aux_state/test_shared_tpch_q12.cpp)
accel_add_shared_interface_test(test_shared_tpch_q14 tests/aux_state/test_shared_tpch_q14.cpp)
accel_add_shared_interface_test(test_shared_sec_edgar_q1 tests/aux_state/test_shared_sec_edgar_q1.cpp)
accel_add_shared_interface_test(test_shared_sec_edgar_q2 tests/aux_state/test_shared_sec_edgar_q2.cpp)
accel_add_shared_interface_test(test_shared_sec_edgar_q3 tests/aux_state/test_shared_sec_edgar_q3.cpp)
accel_add_shared_interface_test(test_shared_sec_edgar_q4 tests/aux_state/test_shared_sec_edgar_q4.cpp)
accel_add_shared_interface_test(test_shared_sec_edgar_q6 tests/aux_state/test_shared_sec_edgar_q6.cpp)
accel_add_shared_interface_test(test_shared_sec_edgar_q24 tests/aux_state/test_shared_q24.cpp)

add_executable(test_shared_incremental_differential
    tests/aux_state/test_shared_incremental_differential.cpp
)

target_link_libraries(test_shared_incremental_differential
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_shared_incremental_differential PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q18_shared_differential
    tests/aux_state/test_tpch_q18_shared_differential.cpp
)

target_link_libraries(test_tpch_q18_shared_differential
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q18_shared_differential PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

if(EXISTS "${ACCEL_Q21_VERUS_LIB}" AND NOT ACCELERATOR_CODEGEN_BUILD_POSTGRES AND NOT ACCELERATOR_CODEGEN_BUILD_MYSQL)
    add_executable(q21_verus_bench_duckdb benchmarks/duckdb/q21_verus_bench.cpp)
    target_link_libraries(q21_verus_bench_duckdb PRIVATE accelerator_codegen duckdb_sibling)
    add_executable(q21_verus_duckdb_fuzz tests/verification/q21verus/q21_verus_duckdb_fuzz.cpp)
    target_link_libraries(q21_verus_duckdb_fuzz PRIVATE accelerator_codegen duckdb_sibling)
endif()

add_executable(test_tpch_shared_differential
    tests/aux_state/test_tpch_shared_differential.cpp
)

target_link_libraries(test_tpch_shared_differential
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_shared_differential PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_shared_tpch_rf_differential
    tests/aux_state/test_shared_tpch_rf_differential.cpp
)

target_link_libraries(test_shared_tpch_rf_differential
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_shared_tpch_rf_differential PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_arg_variants_differential
    tests/aux_state/test_tpch_arg_variants_differential.cpp
    benchmarks/tpch/single-threaded/tpch_arg_variants.cpp
)

target_link_libraries(test_tpch_arg_variants_differential
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_arg_variants_differential
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/tpch/single-threaded
)

add_executable(test_aux_calibration
    tests/aux_state/test_aux_calibration.cpp
)

target_link_libraries(test_aux_calibration
    PRIVATE
        accelerator_codegen
)

target_include_directories(test_aux_calibration PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_shared_duckdb_write_interval
    benchmarks/aux_state/benchmark_shared_duckdb_write_interval.cpp
)

target_link_libraries(benchmark_shared_duckdb_write_interval
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

add_executable(measure_shared_sec_edgar_aux_memory
    benchmarks/aux_state/measure_shared_sec_edgar_aux_memory.cpp
)

target_link_libraries(measure_shared_sec_edgar_aux_memory
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

if(ACCEL_AUX_CALIBRATION)
    add_executable(calibrate_shared_aux_cost_model
        benchmarks/aux_state/calibrate_shared_aux_cost_model.cpp
    )
    target_link_libraries(calibrate_shared_aux_cost_model
        PRIVATE
            accelerator_codegen
            duckdb_sibling
    )

    set(_accel_tpch_calib_db
        "${CMAKE_SOURCE_DIR}/../accelerator-codegen-duckdb/tpch_sf1.duckdb")
    set(_accel_sec_calib_db
        "${CMAKE_SOURCE_DIR}/../accelerator-codegen-duckdb/sec_edgar_sf3.duckdb")
    if(EXISTS "${_accel_tpch_calib_db}" AND EXISTS "${_accel_sec_calib_db}")
        if(ACCEL_CALIBRATE_AT_BUILD)
            add_custom_target(
                calibrate_shared_aux_cost_models
                ALL
                COMMAND "${CMAKE_SOURCE_DIR}/scripts/calibrate_shared_aux_cost_models.sh"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                DEPENDS calibrate_shared_aux_cost_model
                USES_TERMINAL
                COMMENT "Calibrating shared aux cost models for all 12 queries"
            )
        else()
            add_custom_target(
                calibrate_shared_aux_cost_models
                COMMAND "${CMAKE_SOURCE_DIR}/scripts/calibrate_shared_aux_cost_models.sh"
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                DEPENDS calibrate_shared_aux_cost_model
                USES_TERMINAL
                COMMENT "Calibrating shared aux cost models for all 12 queries"
            )
        endif()
    else()
        message(STATUS
            "calibrate_shared_aux_cost_models skipped: full benchmark DBs not found")
    endif()
endif()

add_executable(test_tpch_q17
    tests/single-threaded/test_tpch_q17.cpp
)

target_link_libraries(test_tpch_q17
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q17 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q19
    tests/single-threaded/test_tpch_q19.cpp
)

target_link_libraries(test_tpch_q19
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q19 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q5
    tests/single-threaded/test_tpch_q5.cpp
)

target_link_libraries(test_tpch_q5
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q5 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q7
    tests/single-threaded/test_tpch_q7.cpp
)

target_link_libraries(test_tpch_q7
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q7 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q10
    tests/single-threaded/test_tpch_q10.cpp
)

target_link_libraries(test_tpch_q10
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q10 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q11
    tests/single-threaded/test_tpch_q11.cpp
)

target_link_libraries(test_tpch_q11
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q11 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q13
    tests/single-threaded/test_tpch_q13.cpp
)

target_link_libraries(test_tpch_q13
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q13 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q16
    tests/single-threaded/test_tpch_q16.cpp
)

target_link_libraries(test_tpch_q16
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q16 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q18
    tests/single-threaded/test_tpch_q18.cpp
)

target_link_libraries(test_tpch_q18
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q18 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state/test_cross_accel_correctness.cpp")
    foreach(_aux_gate_test IN ITEMS test_cross_accel_correctness test_cross_accel_isolation test_aux_multi_connection_stress)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state/${_aux_gate_test}.cpp")
            add_executable(${_aux_gate_test}
                tests/aux_state/${_aux_gate_test}.cpp
            )
            target_link_libraries(${_aux_gate_test}
                PRIVATE
                    accelerator_codegen
                    duckdb_sibling
            )
            target_include_directories(${_aux_gate_test} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
            target_sources(${_aux_gate_test} PRIVATE
                tests/test_support/cross_accel_helpers.cpp
            )
            if("${_aux_gate_test}" STREQUAL "test_aux_multi_connection_stress")
                target_sources(${_aux_gate_test} PRIVATE
                    tests/test_support/multi_connection_stress.cpp
                )
            endif()
            add_test(NAME ${_aux_gate_test} COMMAND ${_aux_gate_test})
        endif()
    endforeach()
endif()

add_executable(test_tpch_q22
    tests/single-threaded/test_tpch_q22.cpp
)

target_link_libraries(test_tpch_q22
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q22 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q15
    tests/single-threaded/test_tpch_q15.cpp
)

target_link_libraries(test_tpch_q15
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q15 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q21
    tests/single-threaded/test_tpch_q21.cpp
)

target_link_libraries(test_tpch_q21
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q21 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q20
    tests/single-threaded/test_tpch_q20.cpp
)

target_link_libraries(test_tpch_q20
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q20 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q8
    tests/single-threaded/test_tpch_q8.cpp
)

target_link_libraries(test_tpch_q8
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q8 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_q2
    tests/single-threaded/test_tpch_q2.cpp
)

target_link_libraries(test_tpch_q2
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q6
    benchmarks/tpch/single-threaded/benchmark_q6.cpp
)

target_link_libraries(benchmark_tpch_q6
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q6 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q1
    benchmarks/tpch/single-threaded/benchmark_q1.cpp
)

target_link_libraries(benchmark_tpch_q1
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q1 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q3
    benchmarks/tpch/single-threaded/benchmark_q3.cpp
)

target_link_libraries(benchmark_tpch_q3
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q4
    benchmarks/tpch/single-threaded/benchmark_q4.cpp
)

target_link_libraries(benchmark_tpch_q4
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q4 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q9
    benchmarks/tpch/single-threaded/benchmark_q9.cpp
)

target_link_libraries(benchmark_tpch_q9
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q9 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q12
    benchmarks/tpch/single-threaded/benchmark_q12.cpp
)

target_link_libraries(benchmark_tpch_q12
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q12 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q14
    benchmarks/tpch/single-threaded/benchmark_q14.cpp
)

target_link_libraries(benchmark_tpch_q14
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q14 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q17
    benchmarks/tpch/single-threaded/benchmark_q17.cpp
)

target_link_libraries(benchmark_tpch_q17
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q17 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q19
    benchmarks/tpch/single-threaded/benchmark_q19.cpp
)

target_link_libraries(benchmark_tpch_q19
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q19 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q5
    benchmarks/tpch/single-threaded/benchmark_q5.cpp
)

target_link_libraries(benchmark_tpch_q5
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q5 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q7
    benchmarks/tpch/single-threaded/benchmark_q7.cpp
)

target_link_libraries(benchmark_tpch_q7
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q7 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q10
    benchmarks/tpch/single-threaded/benchmark_q10.cpp
)

target_link_libraries(benchmark_tpch_q10
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q10 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q11
    benchmarks/tpch/single-threaded/benchmark_q11.cpp
)

target_link_libraries(benchmark_tpch_q11
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q11 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q13
    benchmarks/tpch/single-threaded/benchmark_q13.cpp
)

target_link_libraries(benchmark_tpch_q13
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q13 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q16
    benchmarks/tpch/single-threaded/benchmark_q16.cpp
)

target_link_libraries(benchmark_tpch_q16
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q16 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q18
    benchmarks/tpch/single-threaded/benchmark_q18.cpp
)

target_link_libraries(benchmark_tpch_q18
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q18 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q22
    benchmarks/tpch/single-threaded/benchmark_q22.cpp
)

target_link_libraries(benchmark_tpch_q22
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q22 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_stage_c
    benchmarks/tpch/single-threaded/benchmark_stage_c.cpp
)

target_link_libraries(benchmark_tpch_stage_c
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_stage_c PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_breakdown
    benchmarks/tpch/single-threaded/benchmark_tpch_breakdown.cpp
)

target_link_libraries(benchmark_tpch_breakdown
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_breakdown PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_validated
    benchmarks/tpch/single-threaded/benchmark_tpch_validated.cpp
)

target_link_libraries(benchmark_tpch_validated
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_validated PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_rf_delta_vs_rebuild
    benchmarks/tpch/single-threaded/benchmark_tpch_rf_delta_vs_rebuild.cpp
)
target_link_libraries(benchmark_tpch_rf_delta_vs_rebuild
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)
target_include_directories(benchmark_tpch_rf_delta_vs_rebuild
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/tpch/single-threaded
)
set_target_properties(benchmark_tpch_rf_delta_vs_rebuild PROPERTIES
    BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
    INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
)

add_executable(benchmark_tpch_q20_phased_eval
    benchmarks/tpch/single-threaded/benchmark_q20_phased_eval.cpp
)
target_link_libraries(benchmark_tpch_q20_phased_eval
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)
target_include_directories(benchmark_tpch_q20_phased_eval
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/tpch/single-threaded
)
set_target_properties(benchmark_tpch_q20_phased_eval PROPERTIES
    BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
    INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
)

set_target_properties(accelerator_codegen test_registration test_tpch_q6 test_tpch_q1 test_tpch_q3 test_tpch_q4 test_tpch_q9 test_tpch_q12 test_tpch_q14 test_tpch_q17 test_tpch_q19 test_tpch_q5 test_tpch_q7 test_tpch_q10 test_tpch_q11 test_tpch_q13 test_tpch_q16 test_tpch_q18 test_tpch_q22 test_tpch_q15 test_tpch_q21 test_tpch_q20 test_tpch_q8 test_tpch_q2 benchmark_tpch_q6 benchmark_tpch_q1 benchmark_tpch_q3 benchmark_tpch_q4 benchmark_tpch_q9 benchmark_tpch_q12 benchmark_tpch_q14 benchmark_tpch_q17 benchmark_tpch_q19 benchmark_tpch_q5 benchmark_tpch_q7 benchmark_tpch_q10 benchmark_tpch_q11 benchmark_tpch_q13 benchmark_tpch_q16 benchmark_tpch_q18 benchmark_tpch_q22 benchmark_tpch_stage_c benchmark_tpch_breakdown benchmark_tpch_validated PROPERTIES
    BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
    INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
)

if(ACCEL_ENABLE_PARALLEL_TPCH)
	add_executable(test_tpch_parallel
		tests/parallel/test_tpch_parallel.cpp
	)
	target_link_libraries(test_tpch_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q20_semantics
		tests/parallel/test_tpch_q20_semantics.cpp
	)
	target_link_libraries(test_tpch_q20_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q20_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q20_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q20_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q21_semantics
		tests/parallel/test_tpch_q21_semantics.cpp
	)
	target_link_libraries(test_tpch_q21_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q21_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q21_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q21_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q22_semantics
		tests/parallel/test_tpch_q22_semantics.cpp
	)
	target_link_libraries(test_tpch_q22_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q22_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q22_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q22_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q10_semantics
		tests/parallel/test_tpch_q10_semantics.cpp
	)
	target_link_libraries(test_tpch_q10_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q10_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q10_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q10_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q17_semantics
		tests/parallel/test_tpch_q17_semantics.cpp
	)
	target_link_libraries(test_tpch_q17_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q17_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q17_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q17_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q18_semantics
		tests/parallel/test_tpch_q18_semantics.cpp
	)
	target_link_libraries(test_tpch_q18_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q18_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q18_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q18_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q3_semantics
		tests/parallel/test_tpch_q3_semantics.cpp
	)
	target_link_libraries(test_tpch_q3_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q3_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q3_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q3_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q5_semantics
		tests/parallel/test_tpch_q5_semantics.cpp
	)
	target_link_libraries(test_tpch_q5_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q5_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q5_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q5_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q7_semantics
		tests/parallel/test_tpch_q7_semantics.cpp
	)
	target_link_libraries(test_tpch_q7_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q7_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q7_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q7_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q8_semantics
		tests/parallel/test_tpch_q8_semantics.cpp
	)
	target_link_libraries(test_tpch_q8_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q8_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q8_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q8_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q11_semantics
		tests/parallel/test_tpch_q11_semantics.cpp
	)
	target_link_libraries(test_tpch_q11_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q11_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q11_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q11_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q13_semantics
		tests/parallel/test_tpch_q13_semantics.cpp
	)
	target_link_libraries(test_tpch_q13_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q13_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q13_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q13_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q1_semantics
		tests/parallel/test_tpch_q1_semantics.cpp
	)
	target_link_libraries(test_tpch_q1_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q1_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q1_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q1_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q16_semantics
		tests/parallel/test_tpch_q16_semantics.cpp
	)
	target_link_libraries(test_tpch_q16_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q16_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q16_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q16_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(test_tpch_q2_semantics
		tests/parallel/test_tpch_q2_semantics.cpp
	)
	target_link_libraries(test_tpch_q2_semantics PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_tpch_q2_semantics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(test_tpch_q2_semantics PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(test_tpch_q2_semantics PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q5_parallel
		benchmarks/tpch/parallel/benchmark_q5_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q5_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q5_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q5_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q5_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q7_parallel
		benchmarks/tpch/parallel/benchmark_q7_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q7_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q7_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q7_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q7_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q8_parallel
		benchmarks/tpch/parallel/benchmark_q8_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q8_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q8_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q8_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q8_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q1_parallel
		benchmarks/tpch/parallel/benchmark_q1_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q1_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q1_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q1_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q1_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q11_parallel
		benchmarks/tpch/parallel/benchmark_q11_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q11_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q11_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q11_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q11_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q13_parallel
		benchmarks/tpch/parallel/benchmark_q13_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q13_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q13_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q13_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q13_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q16_parallel
		benchmarks/tpch/parallel/benchmark_q16_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q16_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q16_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q16_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q16_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q2_parallel
		benchmarks/tpch/parallel/benchmark_q2_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q2_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q2_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q2_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q2_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q3_parallel
		benchmarks/tpch/parallel/benchmark_q3_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q3_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q3_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q3_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q3_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q18_parallel
		benchmarks/tpch/parallel/benchmark_q18_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q18_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q18_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q18_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q18_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q10_parallel
		benchmarks/tpch/parallel/benchmark_q10_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q10_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q10_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q10_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q10_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q17_parallel
		benchmarks/tpch/parallel/benchmark_q17_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q17_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q17_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q17_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q17_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_q22_parallel
		benchmarks/tpch/parallel/benchmark_q22_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_q22_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_q22_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_q22_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_q22_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_wave1_parallel
		benchmarks/tpch/parallel/benchmark_wave1_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_wave1_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_wave1_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_wave1_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_wave1_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_wave5_parallel
		benchmarks/tpch/parallel/benchmark_wave5_parallel.cpp
	)
	target_link_libraries(benchmark_tpch_wave5_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_tpch_wave5_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	target_compile_definitions(benchmark_tpch_wave5_parallel PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_wave5_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_tpch_scale_proxy
		benchmarks/tpch/parallel/benchmark_scale_proxy.cpp
	)
	target_compile_definitions(benchmark_tpch_scale_proxy PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
	set_target_properties(benchmark_tpch_scale_proxy PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)
endif()

if(ACCEL_ENABLE_PARALLEL_AUX)
	add_executable(test_sec_edgar_aux_parallel
		tests/parallel/test_sec_edgar_aux_parallel.cpp
	)
	target_link_libraries(test_sec_edgar_aux_parallel PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(test_sec_edgar_aux_parallel PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/tests
		${CMAKE_CURRENT_SOURCE_DIR}/src/generated
	)
	target_compile_definitions(test_sec_edgar_aux_parallel PRIVATE ACCEL_ENABLE_PARALLEL_AUX)
	set_target_properties(test_sec_edgar_aux_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	add_executable(benchmark_shared_duckdb_write_interval_parallel
		benchmarks/aux_state/benchmark_shared_duckdb_write_interval_parallel.cpp
	)
	target_link_libraries(benchmark_shared_duckdb_write_interval_parallel
		PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(benchmark_shared_duckdb_write_interval_parallel PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/src/generated
	)
	target_compile_definitions(benchmark_shared_duckdb_write_interval_parallel PRIVATE ACCEL_ENABLE_PARALLEL_AUX)
	set_target_properties(benchmark_shared_duckdb_write_interval_parallel PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)
endif()

if(BUILD_TESTING)
    add_test(NAME registration_smoke_test COMMAND test_registration)
    add_test(NAME tpch_q6_smoke_test COMMAND test_tpch_q6)
    add_test(NAME tpch_q1_smoke_test COMMAND test_tpch_q1)
    add_test(NAME tpch_q3_smoke_test COMMAND test_tpch_q3)
    add_test(NAME tpch_q4_smoke_test COMMAND test_tpch_q4)
    add_test(NAME tpch_q9_smoke_test COMMAND test_tpch_q9)
    add_test(NAME tpch_q12_smoke_test COMMAND test_tpch_q12)
    add_test(NAME tpch_q14_smoke_test COMMAND test_tpch_q14)
    add_test(NAME shared_aux_state_unit_test COMMAND test_shared_aux_state)
    add_test(NAME aux_calibration_unit_test COMMAND test_aux_calibration)
    set(_accel_shared_tpch_db
        "${CMAKE_SOURCE_DIR}/../accelerator-codegen-duckdb/tpch_test.duckdb")
    set(_accel_shared_sec_edgar_db
        "${CMAKE_SOURCE_DIR}/../accelerator-codegen-duckdb/sec_edgar_test.duckdb")
    if(EXISTS "${_accel_shared_tpch_db}" AND EXISTS "${_accel_shared_sec_edgar_db}")
        add_test(
            NAME test_shared_incremental_differential_unit_test
            COMMAND test_shared_incremental_differential
                    "${_accel_shared_tpch_db}" "${_accel_shared_sec_edgar_db}")
        set_tests_properties(
            test_shared_incremental_differential_unit_test
            PROPERTIES RESOURCE_LOCK "tpch_sf1_db;sec_edgar_db")
    endif()
    add_test(NAME tpch_q17_smoke_test COMMAND test_tpch_q17)
    add_test(NAME tpch_q19_smoke_test COMMAND test_tpch_q19)
    add_test(NAME tpch_q5_smoke_test COMMAND test_tpch_q5)
    add_test(NAME tpch_q7_smoke_test COMMAND test_tpch_q7)
    add_test(NAME tpch_q10_smoke_test COMMAND test_tpch_q10)
    add_test(NAME tpch_q11_smoke_test COMMAND test_tpch_q11)
    add_test(NAME tpch_q13_smoke_test COMMAND test_tpch_q13)
    add_test(NAME tpch_q16_smoke_test COMMAND test_tpch_q16)
    add_test(NAME tpch_q18_smoke_test COMMAND test_tpch_q18)
    accel_register_tpch_aux_ctests()
    add_test(NAME tpch_q22_smoke_test COMMAND test_tpch_q22)
    add_test(NAME tpch_q15_smoke_test COMMAND test_tpch_q15)
    add_test(NAME tpch_q21_smoke_test COMMAND test_tpch_q21)
    add_test(NAME tpch_q20_smoke_test COMMAND test_tpch_q20)
    add_test(NAME tpch_q8_smoke_test COMMAND test_tpch_q8)
    add_test(NAME tpch_q2_smoke_test COMMAND test_tpch_q2)
    set_tests_properties(tpch_q6_smoke_test tpch_q1_smoke_test tpch_q3_smoke_test tpch_q4_smoke_test tpch_q9_smoke_test tpch_q12_smoke_test tpch_q14_smoke_test tpch_q17_smoke_test tpch_q19_smoke_test tpch_q5_smoke_test tpch_q7_smoke_test tpch_q10_smoke_test tpch_q11_smoke_test tpch_q13_smoke_test tpch_q16_smoke_test tpch_q18_smoke_test tpch_q22_smoke_test tpch_q15_smoke_test tpch_q21_smoke_test tpch_q20_smoke_test tpch_q8_smoke_test tpch_q2_smoke_test PROPERTIES RESOURCE_LOCK tpch_sf1_db)
	if(ACCEL_ENABLE_PARALLEL_TPCH)
		add_test(NAME tpch_parallel_smoke_test COMMAND test_tpch_parallel)
		add_test(NAME tpch_q20_semantics_test COMMAND test_tpch_q20_semantics)
		add_test(NAME tpch_q21_semantics_test COMMAND test_tpch_q21_semantics)
		add_test(NAME tpch_q22_semantics_test COMMAND test_tpch_q22_semantics)
		add_test(NAME tpch_q10_semantics_test COMMAND test_tpch_q10_semantics)
		add_test(NAME tpch_q17_semantics_test COMMAND test_tpch_q17_semantics)
		add_test(NAME tpch_q18_semantics_test COMMAND test_tpch_q18_semantics)
		add_test(NAME tpch_q3_semantics_test COMMAND test_tpch_q3_semantics)
		add_test(NAME tpch_q2_semantics_test COMMAND test_tpch_q2_semantics)
		add_test(NAME tpch_q5_semantics_test COMMAND test_tpch_q5_semantics)
		add_test(NAME tpch_q7_semantics_test COMMAND test_tpch_q7_semantics)
		add_test(NAME tpch_q8_semantics_test COMMAND test_tpch_q8_semantics)
		add_test(NAME tpch_q11_semantics_test COMMAND test_tpch_q11_semantics)
		add_test(NAME tpch_q13_semantics_test COMMAND test_tpch_q13_semantics)
		add_test(NAME tpch_q1_semantics_test COMMAND test_tpch_q1_semantics)
		add_test(NAME tpch_q16_semantics_test COMMAND test_tpch_q16_semantics)
		set_tests_properties(tpch_parallel_smoke_test tpch_q20_semantics_test tpch_q21_semantics_test tpch_q22_semantics_test tpch_q10_semantics_test tpch_q17_semantics_test tpch_q18_semantics_test tpch_q3_semantics_test tpch_q2_semantics_test tpch_q5_semantics_test tpch_q7_semantics_test tpch_q8_semantics_test tpch_q11_semantics_test tpch_q13_semantics_test tpch_q1_semantics_test tpch_q16_semantics_test PROPERTIES RESOURCE_LOCK tpch_sf1_db)
	endif()
	if(ACCEL_ENABLE_PARALLEL_AUX)
		add_test(NAME sec_edgar_aux_parallel_test COMMAND test_sec_edgar_aux_parallel)
	endif()
endif()

accel_add_base_query(
	BENCHMARK sec_edgar
	QUERY_NUM 1
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
	CTEST_RESOURCE_LOCK sec_edgar_db
)

accel_add_base_query(
	BENCHMARK sec_edgar
	QUERY_NUM 2
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
	CTEST_RESOURCE_LOCK sec_edgar_db
)

accel_add_base_query(
	BENCHMARK sec_edgar
	QUERY_NUM 3
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
	CTEST_RESOURCE_LOCK sec_edgar_db
)

accel_add_base_query(
	BENCHMARK sec_edgar
	QUERY_NUM 4
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
	CTEST_RESOURCE_LOCK sec_edgar_db
)

accel_add_base_query(
	BENCHMARK sec_edgar
	QUERY_NUM 6
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
	CTEST_RESOURCE_LOCK sec_edgar_db
)

accel_add_base_query(
	BENCHMARK sec_edgar
	QUERY_NUM 24
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
	CTEST_RESOURCE_LOCK sec_edgar_db
)

accel_add_aux_state_query(
	BENCHMARK sec_edgar
	QUERY_NUM 3
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
accel_add_aux_state_query(
	BENCHMARK sec_edgar
	QUERY_NUM 4
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
accel_add_aux_state_query(
	BENCHMARK sec_edgar
	QUERY_NUM 6
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)
accel_add_aux_state_query(
	BENCHMARK sec_edgar
	QUERY_NUM 24
	LIBRARY_TARGET accelerator_codegen
	TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
)

# ---------------------------------------------------------------------------
# PostgreSQL TPC-H differential harness
# ---------------------------------------------------------------------------
if(ACCELERATOR_CODEGEN_BUILD_POSTGRES AND ACCEL_PG_FOUND
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state/test_postgres_tpch_differential.cpp")
    add_executable(test_postgres_tpch_differential
        tests/aux_state/test_postgres_tpch_differential.cpp
    )
    find_library(LIBPQ_LIB pq PATHS "${ACCEL_PG_LIBDIR}" NO_DEFAULT_PATH)
    target_link_libraries(test_postgres_tpch_differential
        PRIVATE
            accelerator_codegen
            duckdb_sibling
            ${LIBPQ_LIB}
    )
    target_include_directories(test_postgres_tpch_differential
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
            ${ACCEL_PG_INCLUDEDIR}
            ${ACCEL_PG_INCLUDEDIR_SERVER}
    )
    if(TARGET accelerator_codegen_pg)
        add_dependencies(test_postgres_tpch_differential accelerator_codegen_pg)
    endif()
    if(BUILD_TESTING)
        add_test(NAME postgres_tpch_differential COMMAND test_postgres_tpch_differential)
    endif()

    # Shared PG SQL-vs-accel bench used by baseline / speedup / profile digests.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/postgres/benchmark_postgres_tpch_aux.cpp")
        add_executable(benchmark_postgres_tpch_aux
            benchmarks/postgres/benchmark_postgres_tpch_aux.cpp
        )
        target_link_libraries(benchmark_postgres_tpch_aux
            PRIVATE accelerator_codegen duckdb_sibling)
        target_include_directories(benchmark_postgres_tpch_aux
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
                ${ACCEL_PG_INCLUDEDIR}
                ${ACCEL_PG_INCLUDEDIR_SERVER}
        )
        if(TARGET accelerator_codegen_pg)
            add_dependencies(benchmark_postgres_tpch_aux accelerator_codegen_pg)
        endif()
        # Per-query aliases expected by run_baseline.sh (benchmark_postgres_tpch_qN).
        foreach(q RANGE 1 22)
            add_custom_target(benchmark_postgres_tpch_q${q}
                DEPENDS benchmark_postgres_tpch_aux
                COMMAND ${CMAKE_COMMAND} -E create_symlink
                    $<TARGET_FILE:benchmark_postgres_tpch_aux>
                    ${CMAKE_BINARY_DIR}/benchmark_postgres_tpch_q${q}
            )
        endforeach()
    endif()

    # Postgres TPC-H aux cost-model calibrator (wall-clock via psql \\timing).
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/postgres/calibrate_postgres_tpch_aux_cost_model.cpp")
        add_executable(calibrate_postgres_tpch_aux_cost_model
            benchmarks/postgres/calibrate_postgres_tpch_aux_cost_model.cpp
        )
        target_link_libraries(calibrate_postgres_tpch_aux_cost_model
            PRIVATE accelerator_codegen duckdb_sibling)
        target_include_directories(calibrate_postgres_tpch_aux_cost_model
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state
                ${ACCEL_PG_INCLUDEDIR}
                ${ACCEL_PG_INCLUDEDIR_SERVER}
        )
        if(TARGET accelerator_codegen_pg)
            add_dependencies(calibrate_postgres_tpch_aux_cost_model accelerator_codegen_pg)
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# PostgreSQL aux gate harnesses (multi-connection, cross-accel)
# ---------------------------------------------------------------------------
if(ACCELERATOR_CODEGEN_BUILD_POSTGRES AND ACCEL_PG_FOUND)
    # test_postgres_aux_multi_connection_stress
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state/test_postgres_aux_multi_connection_stress.cpp")
        add_executable(test_postgres_aux_multi_connection_stress
            tests/aux_state/test_postgres_aux_multi_connection_stress.cpp
        )
        target_link_libraries(test_postgres_aux_multi_connection_stress
            PRIVATE
                accelerator_codegen
                duckdb_sibling
        )
        target_include_directories(test_postgres_aux_multi_connection_stress
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
                ${ACCEL_PG_INCLUDEDIR}
                ${ACCEL_PG_INCLUDEDIR_SERVER}
        )
        if(TARGET accelerator_codegen_pg)
            add_dependencies(test_postgres_aux_multi_connection_stress accelerator_codegen_pg)
        endif()
        find_package(Threads REQUIRED)
        target_link_libraries(test_postgres_aux_multi_connection_stress PRIVATE Threads::Threads)
        if(BUILD_TESTING)
            add_test(NAME test_postgres_aux_multi_connection_stress COMMAND test_postgres_aux_multi_connection_stress)
        endif()
    endif()

    # test_postgres_cross_accel
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state/test_postgres_cross_accel.cpp")
        add_executable(test_postgres_cross_accel
            tests/aux_state/test_postgres_cross_accel.cpp
        )
        target_link_libraries(test_postgres_cross_accel
            PRIVATE
                accelerator_codegen
                duckdb_sibling
        )
        target_include_directories(test_postgres_cross_accel
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
                ${ACCEL_PG_INCLUDEDIR}
                ${ACCEL_PG_INCLUDEDIR_SERVER}
        )
        if(TARGET accelerator_codegen_pg)
            add_dependencies(test_postgres_cross_accel accelerator_codegen_pg)
        endif()
        if(BUILD_TESTING)
            add_test(NAME test_postgres_cross_accel COMMAND test_postgres_cross_accel)
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Engine backend layer harnesses (store layer: no PG server headers needed)
# ---------------------------------------------------------------------------
if(ACCELERATOR_CODEGEN_BUILD_POSTGRES)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/engine_backend/test_postgres_store.cpp")
        add_executable(test_postgres_store
            tests/engine_backend/test_postgres_store.cpp
        )
        target_link_libraries(test_postgres_store
            PRIVATE
                accelerator_codegen
                duckdb_sibling
        )
        target_include_directories(test_postgres_store
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
        )
        if(BUILD_TESTING)
            add_test(NAME engine_postgres_store COMMAND test_postgres_store)
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Engine backend layer harnesses (aux_backend layer: PG server headers needed)
# ---------------------------------------------------------------------------
if(ACCELERATOR_CODEGEN_BUILD_POSTGRES)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/engine_backend/test_postgres_aux_backend.cpp")
        add_executable(test_postgres_aux_backend
            tests/engine_backend/test_postgres_aux_backend.cpp
        )
        target_link_libraries(test_postgres_aux_backend
            PRIVATE
                accelerator_codegen
                duckdb_sibling
        )
        target_include_directories(test_postgres_aux_backend
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
        )
        if(BUILD_TESTING)
            add_test(NAME engine_postgres_aux_backend COMMAND test_postgres_aux_backend)
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Engine backend layer harnesses (txn layer: libpq test)
# ---------------------------------------------------------------------------
if(ACCELERATOR_CODEGEN_BUILD_POSTGRES AND ACCEL_PG_FOUND)
    find_library(LIBPQ_LIB pq PATHS "${ACCEL_PG_LIBDIR}" NO_DEFAULT_PATH)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/engine_backend/test_postgres_txn.cpp")
        add_executable(test_postgres_txn
            tests/engine_backend/test_postgres_txn.cpp
        )
        target_include_directories(test_postgres_txn
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
                ${ACCEL_PG_INCLUDEDIR}
        )
        target_link_libraries(test_postgres_txn
            PRIVATE
                ${LIBPQ_LIB}
        )
        if(TARGET accelerator_codegen_pg)
            add_dependencies(test_postgres_txn accelerator_codegen_pg)
        endif()
        if(BUILD_TESTING)
            add_test(NAME engine_postgres_txn COMMAND test_postgres_txn)
        endif()
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/engine_backend/test_postgres_mutations.cpp")
        add_executable(test_postgres_mutations
            tests/engine_backend/test_postgres_mutations.cpp
        )
        target_include_directories(test_postgres_mutations
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
                ${ACCEL_PG_INCLUDEDIR}
        )
        target_link_libraries(test_postgres_mutations
            PRIVATE
                ${LIBPQ_LIB}
        )
        if(TARGET accelerator_codegen_pg)
            add_dependencies(test_postgres_mutations accelerator_codegen_pg)
        endif()
        if(BUILD_TESTING)
            add_test(NAME engine_postgres_mutations COMMAND test_postgres_mutations)
        endif()
    endif()
endif()
# ---------------------------------------------------------------------------
# Engine backend layer harnesses (scan layer: libpq test)
# ---------------------------------------------------------------------------
if(ACCELERATOR_CODEGEN_BUILD_POSTGRES AND ACCEL_PG_FOUND)
    find_library(LIBPQ_LIB pq PATHS "${ACCEL_PG_LIBDIR}" NO_DEFAULT_PATH)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/engine_backend/test_postgres_scan.cpp")
        add_executable(test_postgres_scan
            tests/engine_backend/test_postgres_scan.cpp
        )
        target_include_directories(test_postgres_scan
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
                ${ACCEL_PG_INCLUDEDIR}
        )
        target_link_libraries(test_postgres_scan
            PRIVATE
                ${LIBPQ_LIB}
        )
        if(TARGET accelerator_codegen_pg)
            add_dependencies(test_postgres_scan accelerator_codegen_pg)
        endif()
        if(BUILD_TESTING)
            add_test(NAME engine_postgres_scan COMMAND test_postgres_scan)
        endif()
    endif()
endif()
# ---------------------------------------------------------------------------
# Engine backend layer harnesses (runtime layer: libpq test)
# ---------------------------------------------------------------------------
if(ACCELERATOR_CODEGEN_BUILD_POSTGRES AND ACCEL_PG_FOUND)
    find_library(LIBPQ_LIB pq PATHS "${ACCEL_PG_LIBDIR}" NO_DEFAULT_PATH)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/engine_backend/test_postgres_runtime.cpp")
        add_executable(test_postgres_runtime
            tests/engine_backend/test_postgres_runtime.cpp
        )
        target_include_directories(test_postgres_runtime
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
                ${ACCEL_PG_INCLUDEDIR}
        )
        target_link_libraries(test_postgres_runtime
            PRIVATE
                ${LIBPQ_LIB}
        )
        if(TARGET accelerator_codegen_pg)
            add_dependencies(test_postgres_runtime accelerator_codegen_pg)
        endif()
        if(BUILD_TESTING)
            add_test(NAME engine_postgres_runtime COMMAND test_postgres_runtime)
        endif()
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/engine_backend/test_postgres_visibility.cpp")
        add_executable(test_postgres_visibility
            tests/engine_backend/test_postgres_visibility.cpp
        )
        target_include_directories(test_postgres_visibility
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
                ${ACCEL_PG_INCLUDEDIR}
        )
        target_link_libraries(test_postgres_visibility
            PRIVATE
                ${LIBPQ_LIB}
        )
        if(TARGET accelerator_codegen_pg)
            add_dependencies(test_postgres_visibility accelerator_codegen_pg)
        endif()
        if(BUILD_TESTING)
            add_test(NAME engine_postgres_visibility COMMAND test_postgres_visibility)
        endif()
    endif()
endif()
# ---------------------------------------------------------------------------
# Engine backend layer harnesses (host layer: libpq test)
# ---------------------------------------------------------------------------
if(ACCELERATOR_CODEGEN_BUILD_POSTGRES AND ACCEL_PG_FOUND)
    find_library(LIBPQ_LIB pq PATHS "${ACCEL_PG_LIBDIR}" NO_DEFAULT_PATH)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/engine_backend/test_postgres_host.cpp")
        add_executable(test_postgres_host
            tests/engine_backend/test_postgres_host.cpp
        )
        target_include_directories(test_postgres_host
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
                ${ACCEL_PG_INCLUDEDIR}
        )
        target_link_libraries(test_postgres_host
            PRIVATE
                ${LIBPQ_LIB}
        )
        if(TARGET accelerator_codegen_pg)
            add_dependencies(test_postgres_host accelerator_codegen_pg)
        endif()
        if(BUILD_TESTING)
            add_test(NAME engine_postgres_host COMMAND test_postgres_host)
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Engine backend layer harnesses (MySQL). Early layers link accelerator_codegen
# only; later layers may add mysqlclient / plugin deps in MySQLAdapter.cmake.
# ---------------------------------------------------------------------------
if(ACCELERATOR_CODEGEN_BUILD_MYSQL)
    set(_accel_mysql_layers store aux_backend txn mutations scan runtime visibility host adversarial)
    foreach(_layer IN LISTS _accel_mysql_layers)
        set(_tgt "test_mysql_${_layer}")
        set(_src "${CMAKE_CURRENT_SOURCE_DIR}/tests/engine_backend/${_tgt}.cpp")
        if(EXISTS "${_src}")
            add_executable(${_tgt} tests/engine_backend/${_tgt}.cpp)
            target_link_libraries(${_tgt}
                PRIVATE
                    accelerator_codegen
                    duckdb_sibling
            )
            target_include_directories(${_tgt}
                PRIVATE
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests
            )
            if(ACCEL_MYSQL_FOUND AND ACCEL_MYSQL_INCLUDEDIR)
                target_include_directories(${_tgt} PRIVATE ${ACCEL_MYSQL_INCLUDEDIR})
            endif()
            # txn and later layers use the mysql CLI for real SQL connection harnesses
            # (no libmysqlclient linking needed; the CLI is already in the container)
            if(TARGET accelerator_codegen_mysql)
                add_dependencies(${_tgt} accelerator_codegen_mysql)
            endif()
            if(BUILD_TESTING)
                add_test(NAME engine_mysql_${_layer} COMMAND ${_tgt})
            endif()
        endif()
    endforeach()

    # MySQL TPC-H aux differential / isolation / calibration harnesses
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state/test_mysql_tpch_differential.cpp")
        add_executable(test_mysql_tpch_differential
            tests/aux_state/test_mysql_tpch_differential.cpp
        )
        target_link_libraries(test_mysql_tpch_differential
            PRIVATE accelerator_codegen duckdb_sibling)
        target_include_directories(test_mysql_tpch_differential
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
        )
        if(TARGET accelerator_codegen_mysql)
            add_dependencies(test_mysql_tpch_differential accelerator_codegen_mysql)
        endif()
        if(BUILD_TESTING)
            add_test(NAME mysql_tpch_differential COMMAND test_mysql_tpch_differential)
        endif()
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state/test_mysql_cross_accel.cpp")
        add_executable(test_mysql_cross_accel
            tests/aux_state/test_mysql_cross_accel.cpp
        )
        target_link_libraries(test_mysql_cross_accel
            PRIVATE accelerator_codegen duckdb_sibling)
        target_include_directories(test_mysql_cross_accel
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
        )
        if(TARGET accelerator_codegen_mysql)
            add_dependencies(test_mysql_cross_accel accelerator_codegen_mysql)
        endif()
        if(BUILD_TESTING)
            add_test(NAME test_mysql_cross_accel COMMAND test_mysql_cross_accel)
        endif()
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state/test_mysql_aux_multi_connection_stress.cpp")
        add_executable(test_mysql_aux_multi_connection_stress
            tests/aux_state/test_mysql_aux_multi_connection_stress.cpp
        )
        find_package(Threads REQUIRED)
        target_link_libraries(test_mysql_aux_multi_connection_stress
            PRIVATE accelerator_codegen duckdb_sibling Threads::Threads)
        target_include_directories(test_mysql_aux_multi_connection_stress
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
        )
        if(TARGET accelerator_codegen_mysql)
            add_dependencies(test_mysql_aux_multi_connection_stress accelerator_codegen_mysql)
        endif()
        if(BUILD_TESTING)
            add_test(NAME test_mysql_aux_multi_connection_stress COMMAND test_mysql_aux_multi_connection_stress)
        endif()
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/mysql/benchmark_mysql_tpch_aux.cpp")
        add_executable(benchmark_mysql_tpch_aux
            benchmarks/mysql/benchmark_mysql_tpch_aux.cpp
        )
        target_link_libraries(benchmark_mysql_tpch_aux
            PRIVATE accelerator_codegen duckdb_sibling)
        target_include_directories(benchmark_mysql_tpch_aux
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
        )
        if(TARGET accelerator_codegen_mysql)
            add_dependencies(benchmark_mysql_tpch_aux accelerator_codegen_mysql)
        endif()
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/mysql/calibrate_mysql_tpch_aux_cost_model.cpp")
        add_executable(calibrate_mysql_tpch_aux_cost_model
            benchmarks/mysql/calibrate_mysql_tpch_aux_cost_model.cpp
        )
        target_link_libraries(calibrate_mysql_tpch_aux_cost_model
            PRIVATE accelerator_codegen duckdb_sibling)
        target_include_directories(calibrate_mysql_tpch_aux_cost_model
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
        )
        if(TARGET accelerator_codegen_mysql)
            add_dependencies(calibrate_mysql_tpch_aux_cost_model accelerator_codegen_mysql)
        endif()
    endif()
endif()
