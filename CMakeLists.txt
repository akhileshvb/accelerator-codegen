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

# C++-only aux sources (the generated aux modules and the C++ versioned store) are
# compiled for this machine's CPU (AVX-512 on the Xeon Gold 6230).  Verus kit
# glue, src/verus_kit and the shared table-function/runtime plumbing keep the
# portable flags, so the Verus baseline is unchanged.  Off by default: measured on
# SF1 reads it gave no consistent gain (the module compute is a few us of a ~300 us
# read, the rest is DuckDB's own query machinery) and it makes the library
# CPU-specific.  Opt in with -DACCEL_AUX_COPTS=-march=native.
set(ACCEL_AUX_COPTS "" CACHE STRING "extra compile options for the C++ aux module sources")
if(ACCEL_AUX_COPTS)
    set(_accel_cpp_aux_srcs "")
    foreach(_src IN LISTS ACCEL_GENERATED_SOURCES)
        if(_src MATCHES "/(duckdb/aux|common)/[^/]+\\.cpp$")
            list(APPEND _accel_cpp_aux_srcs "${_src}")
        endif()
    endforeach()
    list(APPEND _accel_cpp_aux_srcs
        "${CMAKE_CURRENT_SOURCE_DIR}/src/stateful/state/versioned_aux_store.cpp")
    set_property(SOURCE ${_accel_cpp_aux_srcs} APPEND PROPERTY COMPILE_OPTIONS ${ACCEL_AUX_COPTS})
endif()

# Stateless base kernels (duckdb/base/{single-threaded,parallel}) use SIMD
# intrinsics guarded by __AVX512F__ / __AVX2__ with scalar fallbacks, so they need
# the target ISA enabled to take the vector paths.  Default -march=native;
# -DACCEL_STATELESS_COPTS= (empty) builds the portable scalar fallbacks.
set(ACCEL_STATELESS_COPTS "-march=native" CACHE STRING "extra compile options for the stateless base kernel sources")
function(accel_apply_stateless_copts)
    if(NOT ACCEL_STATELESS_COPTS)
        return()
    endif()
    set(_srcs "")
    foreach(_src IN LISTS ARGN)
        if(_src MATCHES "/duckdb/base/")
            list(APPEND _srcs "${_src}")
        endif()
    endforeach()
    if(_srcs)
        separate_arguments(_opts UNIX_COMMAND "${ACCEL_STATELESS_COPTS}")
        set_property(SOURCE ${_srcs} APPEND PROPERTY COMPILE_OPTIONS ${_opts})
    endif()
endfunction()
accel_apply_stateless_copts(${ACCEL_GENERATED_SOURCES})

# SEC-EDGAR accelerators (shared state, DuckDB aux, Postgres aux/extension) are
# always compiled for this machine's CPU, so every loop can use AVX-512/AVX2/BMI,
# not just the hand-written kernels behind __builtin_cpu_supports. Source
# properties are directory-scoped, so this also reaches accelerator_codegen_pg.
# The DuckDB base kernels already get ACCEL_STATELESS_COPTS. The library becomes
# CPU-specific; -DACCEL_SEC_EDGAR_COPTS= (empty) restores the portable build.
set(ACCEL_SEC_EDGAR_COPTS "-march=native" CACHE STRING "extra compile options for the SEC-EDGAR accelerator sources")
if(ACCEL_SEC_EDGAR_COPTS)
    file(GLOB_RECURSE _accel_sec_edgar_srcs CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/sec_edgar/*.cpp")
    list(FILTER _accel_sec_edgar_srcs EXCLUDE REGEX "/duckdb/base/")
    if(_accel_sec_edgar_srcs)
        separate_arguments(_opts UNIX_COMMAND "${ACCEL_SEC_EDGAR_COPTS}")
        set_property(SOURCE ${_accel_sec_edgar_srcs} APPEND PROPERTY COMPILE_OPTIONS ${_opts})
    endif()
endif()

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

# Verus accelerators through the host kit (include/accelerator_codegen/verus/
# verus_kit.hpp): each src/generated/<bench>/q<N>/verus_kit/<crate>/ holds the query's host
# glue (*.cpp: the pipeline's glue.cpp, or sql2verus's gen_glue.cpp) and the
# kernel's C header; the kernel is
# ${ACCEL_VERUS_LIB_DIR}/lib<crate>_verus.a (scripts/build_verus_crate.sh).
# A glue registers itself, so nothing shared names the query.
set(ACCEL_VERUS_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build-verus"
    CACHE PATH "Directory of Verus kernel static libraries (lib<crate>_verus.a)")
# A benchmark's Verus world (scripts/gen_verus_world.py: one verified store for
# all its kits, ACCEL_AUX_STORE=verus_global) is
# src/generated/<bench>/verus_world/<world>/ (world_glue.cpp) and
# ${ACCEL_VERUS_LIB_DIR}/lib<world>_verus.a (scripts/build_verus_world.sh), an
# archive holding every kit it names: linked instead of the kits' own.
file(GLOB _verus_world_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/*/verus_world/*")
set(_verus_world_benches)
foreach(_dir IN LISTS _verus_world_dirs)
    get_filename_component(_world "${_dir}" NAME)
    set(_lib "${ACCEL_VERUS_LIB_DIR}/lib${_world}_verus.a")
    if(IS_DIRECTORY "${_dir}" AND EXISTS "${_lib}" AND EXISTS "${_dir}/world_glue.cpp")
        string(REGEX MATCH "src/generated/([^/]+)/verus_world" _m "${_dir}")
        list(APPEND _verus_world_benches "${CMAKE_MATCH_1}")
        message(STATUS "Verus world: ${_world}")
        target_sources(accelerator_codegen PRIVATE "${_dir}/world_glue.cpp")
        set_source_files_properties("${_dir}/world_glue.cpp" PROPERTIES INCLUDE_DIRECTORIES "${_dir}")
        target_link_libraries(accelerator_codegen PRIVATE "${_lib}" pthread dl m)
    endif()
endforeach()
file(GLOB _verus_kit_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/*/q*/verus_kit/*")
# ACCEL_VERUS_ONLY_KITS: a ;-list of kit crates (e.g. tpch_q9_vgen) whose glue
# is built; the others are left out (unregistered).  For working on one kit
# while other kits' sources are mid-change.  Empty (default): every kit.
set(ACCEL_VERUS_ONLY_KITS "" CACHE STRING "Build only these Verus kits' glue (;-list); empty = all")
foreach(_dir IN LISTS _verus_kit_dirs)
    file(GLOB _verus_kit_srcs CONFIGURE_DEPENDS "${_dir}/*.cpp")
    get_filename_component(_crate "${_dir}" NAME)
    if(ACCEL_VERUS_ONLY_KITS AND NOT _crate IN_LIST ACCEL_VERUS_ONLY_KITS)
        continue()
    endif()
    if(IS_DIRECTORY "${_dir}" AND _verus_kit_srcs)
        set(_lib "${ACCEL_VERUS_LIB_DIR}/lib${_crate}_verus.a")
        string(REGEX MATCH "src/generated/([^/]+)/q" _m "${_dir}")
        set(_in_world FALSE)
        if("${CMAKE_MATCH_1}" IN_LIST _verus_world_benches)
            set(_in_world TRUE)
        endif()
        if(EXISTS "${_lib}" OR _in_world)
            message(STATUS "Verus kit accelerator: ${_crate}")
            target_sources(accelerator_codegen PRIVATE ${_verus_kit_srcs})
            set_source_files_properties(${_verus_kit_srcs} PROPERTIES INCLUDE_DIRECTORIES "${_dir}")
            if(NOT _in_world)
                target_link_libraries(accelerator_codegen PRIVATE "${_lib}" pthread dl m)
            endif()
        endif()
    endif()
endforeach()

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
		accel_apply_stateless_copts(${ACCEL_GENERATED_PARALLEL_SOURCES})
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

# Test / benchmark executable linking the library and DuckDB, with tests/ on
# the include path.  Extra arguments are the sources.
function(accel_add_duckdb_exe name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE accelerator_codegen duckdb_sibling)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
endfunction()

# Same, for the multithreaded TPC-H variants (ACCEL_ENABLE_PARALLEL_TPCH).
function(accel_add_parallel_tpch_exe name)
    accel_add_duckdb_exe(${name} ${ARGN})
    target_compile_definitions(${name} PRIVATE ACCEL_ENABLE_PARALLEL_TPCH)
    set_target_properties(${name} PROPERTIES
        BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
        INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}")
endfunction()

# Per-query TPC-H smoke tests (tests/single-threaded/test_tpch_q<N>.cpp) and
# single-threaded benchmarks (benchmarks/tpch/single-threaded/benchmark_q<N>.cpp).
set(ACCEL_TPCH_SMOKE_QS 6 1 3 4 9 12 14 17 19 5 7 10 11 13 16 18 22 15 21 20 8 2)
set(ACCEL_TPCH_BENCH_QS 6 1 3 4 9 12 14 17 19 5 7 10 11 13 16 18 22)
foreach(_q IN LISTS ACCEL_TPCH_SMOKE_QS)
    accel_add_duckdb_exe(test_tpch_q${_q} tests/single-threaded/test_tpch_q${_q}.cpp)
endforeach()
accel_add_duckdb_exe(test_shared_aux_state tests/aux_state/test_shared_aux_state.cpp)
add_executable(test_versioned_aux_state
    tests/aux_state/test_versioned_aux_state.cpp
)
target_link_libraries(test_versioned_aux_state
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)
if(BUILD_TESTING)
    add_test(NAME versioned_aux_state_unit_test COMMAND test_versioned_aux_state)
endif()

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
accel_add_shared_interface_test(test_shared_sec_edgar_q1_random tests/aux_state/test_shared_sec_edgar_q1_random.cpp)
accel_add_shared_interface_test(test_sec_edgar_q1_units tests/aux_state/test_sec_edgar_q1_units.cpp)
target_include_directories(test_sec_edgar_q1_units PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/generated)
accel_add_shared_interface_test(test_sec_edgar_q1_duckdb_differential tests/aux_state/test_sec_edgar_q1_duckdb_differential.cpp)
add_executable(test_sec_edgar_q1_shared_differential tests/aux_state/test_sec_edgar_q1_shared_differential.cpp)
target_link_libraries(test_sec_edgar_q1_shared_differential PRIVATE accelerator_codegen duckdb_sibling)
add_executable(benchmark_sec_edgar_q1_micro benchmarks/aux_state/benchmark_sec_edgar_q1_micro.cpp)
target_link_libraries(benchmark_sec_edgar_q1_micro PRIVATE accelerator_codegen duckdb_sibling)
accel_add_shared_interface_test(test_shared_sec_edgar_q2 tests/aux_state/test_shared_sec_edgar_q2.cpp)
accel_add_shared_interface_test(test_shared_sec_edgar_q3 tests/aux_state/test_shared_sec_edgar_q3.cpp)

# Q3: randomized differential test of the aux accelerator vs plain DuckDB SQL
# (in-memory synthetic data, production DDL; no external database needed).
add_executable(test_sec_edgar_q3_differential tests/aux_state/test_sec_edgar_q3_differential.cpp)
target_link_libraries(test_sec_edgar_q3_differential PRIVATE accelerator_codegen duckdb_sibling)
target_include_directories(test_sec_edgar_q3_differential PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
if(ACCEL_ENABLE_PARALLEL_AUX)
    target_compile_definitions(test_sec_edgar_q3_differential PRIVATE ACCEL_ENABLE_PARALLEL_AUX)
endif()
if(BUILD_TESTING)
    add_test(NAME test_sec_edgar_q3_differential
             COMMAND test_sec_edgar_q3_differential 300 3
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
add_executable(bench_sec_edgar_q3_aux benchmarks/aux_state/bench_sec_edgar_q3_aux.cpp)
target_link_libraries(bench_sec_edgar_q3_aux PRIVATE accelerator_codegen duckdb_sibling)
accel_add_shared_interface_test(test_shared_sec_edgar_q4 tests/aux_state/test_shared_sec_edgar_q4.cpp)
accel_add_shared_interface_test(test_shared_sec_edgar_q6 tests/aux_state/test_shared_sec_edgar_q6.cpp)
add_executable(bench_q6_maintenance benchmarks/sec_edgar/bench_q6_maintenance.cpp)
target_link_libraries(bench_q6_maintenance PRIVATE accelerator_codegen duckdb_sibling)
target_include_directories(bench_q6_maintenance PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_executable(bench_q6_structural benchmarks/sec_edgar/bench_q6_structural.cpp)
target_link_libraries(bench_q6_structural PRIVATE accelerator_codegen duckdb_sibling)
target_include_directories(bench_q6_structural PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_executable(measure_q6_aux_heap benchmarks/sec_edgar/measure_q6_aux_heap.cpp)
target_link_libraries(measure_q6_aux_heap PRIVATE accelerator_codegen duckdb_sibling)
target_include_directories(measure_q6_aux_heap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
accel_add_shared_interface_test(test_sec_edgar_q6_state tests/aux_state/test_sec_edgar_q6_state.cpp)
accel_add_shared_interface_test(test_sec_edgar_q6_differential tests/aux_state/test_sec_edgar_q6_differential.cpp)
if(BUILD_TESTING)
    # The differential tests read config/*.json relative to the source root.
    set_tests_properties(test_sec_edgar_q6_state_unit_test PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(test_sec_edgar_q6_differential_unit_test PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    foreach(_q6_case IN ITEMS "synthetic;11;400" "synthetic;12;400;parallel" "synthetic-large;13;250")
        list(GET _q6_case 0 _q6_mode)
        list(GET _q6_case 1 _q6_seed)
        string(REPLACE ";" "_" _q6_name "${_q6_case}")
        add_test(NAME sec_edgar_q6_differential_${_q6_name}
                 COMMAND test_sec_edgar_q6_differential ${_q6_case}
                 WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endforeach()
endif()
accel_add_shared_interface_test(test_shared_sec_edgar_q24 tests/aux_state/test_shared_q24.cpp)
accel_add_shared_interface_test(test_sec_edgar_q24_duckdb_differential tests/aux_state/test_sec_edgar_q24_duckdb_differential.cpp)

add_executable(benchmark_sec_edgar_q24_micro
    benchmarks/aux_state/benchmark_sec_edgar_q24_micro.cpp
)
target_link_libraries(benchmark_sec_edgar_q24_micro PRIVATE accelerator_codegen duckdb_sibling)

accel_add_duckdb_exe(test_shared_incremental_differential tests/aux_state/test_shared_incremental_differential.cpp)
accel_add_duckdb_exe(test_tpch_q18_shared_differential tests/aux_state/test_tpch_q18_shared_differential.cpp)
# Generic test / bench runner for Verus accelerators of the host kit (needs no
# particular accelerator: the Verus pipeline builds it in a tree with none).
if(NOT ACCELERATOR_CODEGEN_BUILD_POSTGRES AND NOT ACCELERATOR_CODEGEN_BUILD_MYSQL)
    add_executable(verus_kit_runner tests/verification/verus_kit/verus_kit_runner.cpp)
    target_link_libraries(verus_kit_runner PRIVATE accelerator_codegen duckdb_sibling)
endif()

accel_add_duckdb_exe(test_tpch_shared_differential tests/aux_state/test_tpch_shared_differential.cpp)
accel_add_duckdb_exe(test_shared_tpch_rf_differential tests/aux_state/test_shared_tpch_rf_differential.cpp)
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

# Generic core primitives (DenseIdArray, ChunkedCowArray, FlatHashMap, sql_date,
# sql_like, StringBlob/StringDict, int128 helpers). Header-only; no DBMS needed.
find_package(Threads REQUIRED)
add_executable(test_core_primitives
    tests/aux_state/test_core_primitives.cpp
)
target_include_directories(test_core_primitives PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_core_primitives PRIVATE Threads::Threads)
if(BUILD_TESTING)
    add_test(NAME core_primitives_unit_test COMMAND test_core_primitives)
endif()

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

add_executable(bench_sec_edgar_batch_apply
    benchmarks/aux_state/bench_sec_edgar_batch_apply.cpp
)
target_link_libraries(bench_sec_edgar_batch_apply PRIVATE accelerator_codegen duckdb_sibling)

accel_add_duckdb_exe(test_sec_edgar_q4_duckdb_differential tests/aux_state/test_sec_edgar_q4_duckdb_differential.cpp)
if(ACCEL_ENABLE_PARALLEL_AUX)
    target_compile_definitions(test_sec_edgar_q4_duckdb_differential PRIVATE ACCEL_ENABLE_PARALLEL_AUX)
endif()

add_executable(measure_shared_sec_edgar_aux_memory
    benchmarks/aux_state/measure_shared_sec_edgar_aux_memory.cpp
)

target_link_libraries(measure_shared_sec_edgar_aux_memory
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

# C++ vs Verus aux accelerators: read-only and concurrent reader/writer runs
# (benchmarks/compare/run_compare.py).
add_executable(aux_compare benchmarks/compare/aux_compare.cpp)
target_link_libraries(aux_compare PRIVATE accelerator_codegen duckdb_sibling)

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
        "${DUCKDB_ROOT}/tpch_sf1.duckdb")
    set(_accel_sec_calib_db
        "${DUCKDB_ROOT}/sec_edgar_sf3.duckdb")
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

# Randomized differential fuzz of the TPC-H aux accelerators with engine
# capture (tests/aux_state/test_tpch_aux_fuzz.cpp; run by hand, needs a DB).
# Cost-model choice vs measured action costs under different writes
# (tests/aux_state/maintenance_choice_demo.cpp; run by hand on a TPC-H DB).
add_executable(maintenance_choice_demo
    tests/aux_state/maintenance_choice_demo.cpp
    tests/test_support/cross_accel_helpers.cpp
)
target_link_libraries(maintenance_choice_demo PRIVATE accelerator_codegen duckdb_sibling)
target_include_directories(maintenance_choice_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test_tpch_aux_fuzz
    tests/aux_state/test_tpch_aux_fuzz.cpp
    tests/test_support/cross_accel_helpers.cpp
)
target_link_libraries(test_tpch_aux_fuzz PRIVATE accelerator_codegen duckdb_sibling)
target_include_directories(test_tpch_aux_fuzz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

foreach(_q IN LISTS ACCEL_TPCH_BENCH_QS)
    accel_add_duckdb_exe(benchmark_tpch_q${_q} benchmarks/tpch/single-threaded/benchmark_q${_q}.cpp)
endforeach()
accel_add_duckdb_exe(benchmark_tpch_stage_c benchmarks/tpch/single-threaded/benchmark_stage_c.cpp)
accel_add_duckdb_exe(benchmark_tpch_breakdown benchmarks/tpch/single-threaded/benchmark_tpch_breakdown.cpp)
accel_add_duckdb_exe(benchmark_tpch_validated benchmarks/tpch/single-threaded/benchmark_tpch_validated.cpp)
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

set(_accel_rpath_targets accelerator_codegen test_registration benchmark_tpch_stage_c benchmark_tpch_breakdown benchmark_tpch_validated)
foreach(_q IN LISTS ACCEL_TPCH_SMOKE_QS)
    list(APPEND _accel_rpath_targets test_tpch_q${_q})
endforeach()
foreach(_q IN LISTS ACCEL_TPCH_BENCH_QS)
    list(APPEND _accel_rpath_targets benchmark_tpch_q${_q})
endforeach()
set_target_properties(${_accel_rpath_targets} PROPERTIES
    BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
    INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
)

if(ACCEL_ENABLE_PARALLEL_TPCH)
	# Single-threaded + parallel stateless kernels vs reference SQL, all 22 queries.
	add_executable(stateless_matrix
		benchmarks/tpch/stateless_matrix.cpp
	)
	target_link_libraries(stateless_matrix PRIVATE accelerator_codegen duckdb_sibling)
	target_include_directories(stateless_matrix PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
	set_target_properties(stateless_matrix PROPERTIES
		BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
		INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
	)

	accel_add_parallel_tpch_exe(test_tpch_parallel tests/parallel/test_tpch_parallel.cpp)
	foreach(_q 20 21 22 10 17 18 3 5 7 8 11 13 1 16 2)
		if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/parallel/test_tpch_q${_q}_semantics.cpp")
			accel_add_parallel_tpch_exe(test_tpch_q${_q}_semantics tests/parallel/test_tpch_q${_q}_semantics.cpp)
		endif()
	endforeach()
	foreach(_q 5 7 8 1 11 13 16 2 3 18 10 17 22)
		if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/tpch/parallel/benchmark_q${_q}_parallel.cpp")
			accel_add_parallel_tpch_exe(benchmark_tpch_q${_q}_parallel benchmarks/tpch/parallel/benchmark_q${_q}_parallel.cpp)
		endif()
	endforeach()
	accel_add_parallel_tpch_exe(benchmark_tpch_wave1_parallel benchmarks/tpch/parallel/benchmark_wave1_parallel.cpp)
	accel_add_parallel_tpch_exe(benchmark_tpch_wave5_parallel benchmarks/tpch/parallel/benchmark_wave5_parallel.cpp)
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
    foreach(_q IN LISTS ACCEL_TPCH_SMOKE_QS)
        add_test(NAME tpch_q${_q}_smoke_test COMMAND test_tpch_q${_q})
        set_tests_properties(tpch_q${_q}_smoke_test PROPERTIES RESOURCE_LOCK tpch_sf1_db)
    endforeach()
    add_test(NAME shared_aux_state_unit_test COMMAND test_shared_aux_state)
    add_test(NAME aux_calibration_unit_test COMMAND test_aux_calibration)
    set(_accel_shared_tpch_db
        "${DUCKDB_ROOT}/tpch_test.duckdb")
    set(_accel_shared_sec_edgar_db
        "${DUCKDB_ROOT}/sec_edgar_test.duckdb")
    if(EXISTS "${_accel_shared_tpch_db}" AND EXISTS "${_accel_shared_sec_edgar_db}")
        add_test(
            NAME test_shared_incremental_differential_unit_test
            COMMAND test_shared_incremental_differential
                    "${_accel_shared_tpch_db}" "${_accel_shared_sec_edgar_db}")
        set_tests_properties(
            test_shared_incremental_differential_unit_test
            PROPERTIES RESOURCE_LOCK "tpch_sf1_db;sec_edgar_db")
    endif()
    accel_register_tpch_aux_ctests()
	if(ACCEL_ENABLE_PARALLEL_TPCH)
		add_test(NAME tpch_parallel_smoke_test COMMAND test_tpch_parallel)
		set_tests_properties(tpch_parallel_smoke_test PROPERTIES RESOURCE_LOCK tpch_sf1_db)
		foreach(_q 20 21 22 10 17 18 3 2 5 7 8 11 13 1 16)
			if(TARGET test_tpch_q${_q}_semantics)
				add_test(NAME tpch_q${_q}_semantics_test COMMAND test_tpch_q${_q}_semantics)
				set_tests_properties(tpch_q${_q}_semantics_test PROPERTIES RESOURCE_LOCK tpch_sf1_db)
			endif()
		endforeach()
	endif()
	if(ACCEL_ENABLE_PARALLEL_AUX)
		add_test(NAME sec_edgar_aux_parallel_test COMMAND test_sec_edgar_aux_parallel)
	endif()
endif()

foreach(_q 1 2 3 4 6 24)
	accel_add_base_query(
		BENCHMARK sec_edgar
		QUERY_NUM ${_q}
		LIBRARY_TARGET accelerator_codegen
		TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
		CTEST_RESOURCE_LOCK sec_edgar_db
	)
endforeach()

foreach(_q 3 4 6 24)
	accel_add_aux_state_query(
		BENCHMARK sec_edgar
		QUERY_NUM ${_q}
		LIBRARY_TARGET accelerator_codegen
		TEST_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/tests
	)
endforeach()

# SEC EDGAR Q6 on PostgreSQL: libpq-only differential / SF3 check / benchmark
# against a running server (see the file header for modes).
if(ACCELERATOR_CODEGEN_BUILD_POSTGRES AND ACCEL_PG_FOUND
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state/test_sec_edgar_q6_postgres_differential.cpp")
    find_library(_Q6_PG_LIBPQ pq PATHS "${ACCEL_PG_LIBDIR}" NO_DEFAULT_PATH)
    add_executable(test_sec_edgar_q6_postgres_differential
        tests/aux_state/test_sec_edgar_q6_postgres_differential.cpp)
    target_link_libraries(test_sec_edgar_q6_postgres_differential PRIVATE ${_Q6_PG_LIBPQ})
    target_include_directories(test_sec_edgar_q6_postgres_differential PRIVATE ${ACCEL_PG_INCLUDEDIR})
endif()

# ---------------------------------------------------------------------------
# PostgreSQL TPC-H differential harness
# ---------------------------------------------------------------------------
if(ACCELERATOR_CODEGEN_BUILD_POSTGRES AND ACCEL_PG_FOUND
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state/test_postgres_tpch_differential.cpp")
    # postgres_server_stubs.cpp: server-only symbols the library references.
    add_executable(test_postgres_tpch_fuzz
        tests/aux_state/test_postgres_tpch_fuzz.cpp
        tests/aux_state/postgres_server_stubs.cpp
    )
    find_library(LIBPQ_FUZZ_LIB pq PATHS "${ACCEL_PG_LIBDIR}" NO_DEFAULT_PATH)
    target_link_libraries(test_postgres_tpch_fuzz
        PRIVATE
            accelerator_codegen
            duckdb_sibling
            ${LIBPQ_FUZZ_LIB}
    )
    target_include_directories(test_postgres_tpch_fuzz
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
            ${ACCEL_PG_INCLUDEDIR}
    )
    # Parameter-generality differential (all 22 queries, spec / off-spec / edge params).
    add_executable(test_postgres_tpch_params
        tests/aux_state/test_postgres_tpch_params.cpp
        tests/aux_state/postgres_server_stubs.cpp
    )
    find_library(LIBPQ_PARAMS_LIB pq PATHS "${ACCEL_PG_LIBDIR}" NO_DEFAULT_PATH)
    target_link_libraries(test_postgres_tpch_params
        PRIVATE
            accelerator_codegen
            duckdb_sibling
            ${LIBPQ_PARAMS_LIB}
    )
    target_include_directories(test_postgres_tpch_params
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
            ${ACCEL_PG_INCLUDEDIR}
    )
    # Shared accelerator state (accel.shared_state): hits across connections,
    # memory vs connections, concurrency, killed backends, worker restarts.
    add_executable(test_postgres_shared_state
        tests/aux_state/test_postgres_shared_state.cpp
        tests/aux_state/postgres_server_stubs.cpp
    )
    find_library(LIBPQ_SHARED_LIB pq PATHS "${ACCEL_PG_LIBDIR}" NO_DEFAULT_PATH)
    find_package(Threads REQUIRED)
    target_link_libraries(test_postgres_shared_state
        PRIVATE
            accelerator_codegen
            duckdb_sibling
            ${LIBPQ_SHARED_LIB}
            Threads::Threads
    )
    target_include_directories(test_postgres_shared_state
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
            ${ACCEL_PG_INCLUDEDIR}
    )
    add_executable(test_postgres_tpch_differential
        tests/aux_state/test_postgres_tpch_differential.cpp
        tests/aux_state/postgres_server_stubs.cpp
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

    # SEC EDGAR Q1 on PostgreSQL: differential (random / concurrent / sf3) and benchmark.
    # Plain libpq clients; they need the extension installed in the target database.
    foreach(_sec_q1_target IN ITEMS test_postgres_sec_edgar_q1_differential benchmark_postgres_sec_edgar_q1)
        if(_sec_q1_target STREQUAL "test_postgres_sec_edgar_q1_differential")
            set(_sec_q1_src tests/aux_state/test_postgres_sec_edgar_q1_differential.cpp)
        else()
            set(_sec_q1_src benchmarks/postgres/benchmark_postgres_sec_edgar_q1.cpp)
        endif()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_sec_q1_src}")
            add_executable(${_sec_q1_target} ${_sec_q1_src})
            find_library(LIBPQ_SEC_Q1_LIB pq PATHS "${ACCEL_PG_LIBDIR}" NO_DEFAULT_PATH)
            find_package(Threads REQUIRED)
            target_link_libraries(${_sec_q1_target} PRIVATE ${LIBPQ_SEC_Q1_LIB} Threads::Threads)
            target_include_directories(${_sec_q1_target} PRIVATE ${ACCEL_PG_INCLUDEDIR})
        endif()
    endforeach()

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
            tests/aux_state/postgres_server_stubs.cpp
        )
        find_library(LIBPQ_CALIB_LIB pq PATHS "${ACCEL_PG_LIBDIR}" NO_DEFAULT_PATH)
        target_link_libraries(calibrate_postgres_tpch_aux_cost_model
            PRIVATE accelerator_codegen duckdb_sibling ${LIBPQ_CALIB_LIB})
        target_include_directories(calibrate_postgres_tpch_aux_cost_model
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
                ${ACCEL_PG_INCLUDEDIR}
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
            tests/aux_state/postgres_server_stubs.cpp
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
            tests/aux_state/postgres_server_stubs.cpp
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
# PostgreSQL engine backend layer harnesses. store / aux_backend link the
# library; the later layers are plain libpq tests against the PG server.
# ---------------------------------------------------------------------------
if(ACCELERATOR_CODEGEN_BUILD_POSTGRES)
    foreach(_layer IN ITEMS store aux_backend)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/engine_backend/test_postgres_${_layer}.cpp")
            add_executable(test_postgres_${_layer}
                tests/engine_backend/test_postgres_${_layer}.cpp
            )
            target_link_libraries(test_postgres_${_layer}
                PRIVATE
                    accelerator_codegen
                    duckdb_sibling
            )
            target_include_directories(test_postgres_${_layer}
                PRIVATE
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests
            )
            if(BUILD_TESTING)
                add_test(NAME engine_postgres_${_layer} COMMAND test_postgres_${_layer})
            endif()
        endif()
    endforeach()
endif()

if(ACCELERATOR_CODEGEN_BUILD_POSTGRES AND ACCEL_PG_FOUND)
    find_library(LIBPQ_LIB pq PATHS "${ACCEL_PG_LIBDIR}" NO_DEFAULT_PATH)
    foreach(_layer IN ITEMS txn mutations scan runtime visibility host)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/engine_backend/test_postgres_${_layer}.cpp")
            add_executable(test_postgres_${_layer}
                tests/engine_backend/test_postgres_${_layer}.cpp
            )
            target_include_directories(test_postgres_${_layer}
                PRIVATE
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests
                    ${ACCEL_PG_INCLUDEDIR}
            )
            target_link_libraries(test_postgres_${_layer}
                PRIVATE
                    ${LIBPQ_LIB}
            )
            if(TARGET accelerator_codegen_pg)
                add_dependencies(test_postgres_${_layer} accelerator_codegen_pg)
            endif()
            if(BUILD_TESTING)
                add_test(NAME engine_postgres_${_layer} COMMAND test_postgres_${_layer})
            endif()
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Engine backend layer harnesses and TPC-H aux harnesses (MySQL). Early layers
# link accelerator_codegen only; later layers may add mysqlclient / plugin deps
# in MySQLAdapter.cmake.
# ---------------------------------------------------------------------------
if(ACCELERATOR_CODEGEN_BUILD_MYSQL)
    # The MySQL host state (scan helper, query catalog) is part of
    # accelerator_codegen only when it is built for the mysql engine (the
    # container build, ACCEL_CODEGEN_ENGINE=mysql). For any other engine the
    # harnesses get it from a small separate library; none of it needs the
    # plugin SDK.
    accel_codegen_engine_name(_mysql_test_engine)
    set(ACCEL_MYSQL_TEST_LIBS accelerator_codegen duckdb_sibling)
    if(NOT _mysql_test_engine STREQUAL "mysql"
       AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/mysql/scan_helper.cpp"
       AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/generated/catalog/mysql/query_catalog.cpp")
        add_library(accelerator_codegen_mysql_host STATIC
            src/runtime/mysql/scan_helper.cpp
            src/generated/catalog/mysql/query_catalog.cpp
        )
        target_include_directories(accelerator_codegen_mysql_host PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
        )
        target_compile_features(accelerator_codegen_mysql_host PUBLIC cxx_std_17)
        set(ACCEL_MYSQL_TEST_LIBS accelerator_codegen_mysql_host accelerator_codegen duckdb_sibling)
    endif()

    # accel_add_mysql_test(<target> <source> [extra link libs...])
    function(accel_add_mysql_test name src)
        add_executable(${name} ${src})
        target_link_libraries(${name} PRIVATE ${ACCEL_MYSQL_TEST_LIBS} ${ARGN})
        target_include_directories(${name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
        )
        if(ACCEL_MYSQL_FOUND AND ACCEL_MYSQL_INCLUDEDIR)
            target_include_directories(${name} PRIVATE ${ACCEL_MYSQL_INCLUDEDIR})
        endif()
        # The harnesses use the mysql CLI for real SQL connections (no
        # libmysqlclient linking; the CLI is already in the container).
        if(TARGET accelerator_codegen_mysql)
            add_dependencies(${name} accelerator_codegen_mysql)
        endif()
    endfunction()

    foreach(_layer IN ITEMS store aux_backend txn mutations scan runtime visibility host adversarial)
        set(_src "tests/engine_backend/test_mysql_${_layer}.cpp")
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_src}")
            accel_add_mysql_test(test_mysql_${_layer} ${_src})
            if(BUILD_TESTING)
                add_test(NAME engine_mysql_${_layer} COMMAND test_mysql_${_layer})
            endif()
        endif()
    endforeach()

    # TPC-H aux differential / parameter-generality / isolation / calibration harnesses
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state/test_mysql_tpch_differential.cpp")
        accel_add_mysql_test(test_mysql_tpch_fuzz tests/aux_state/test_mysql_tpch_fuzz.cpp)
        # all 22 queries; spec / off-spec / edge / NULL params; plain, data-perturb,
        # wide-keys, negative-keys modes
        accel_add_mysql_test(test_mysql_tpch_params tests/aux_state/test_mysql_tpch_params.cpp)
        accel_add_mysql_test(test_mysql_tpch_differential tests/aux_state/test_mysql_tpch_differential.cpp)
        if(BUILD_TESTING)
            add_test(NAME mysql_tpch_differential COMMAND test_mysql_tpch_differential)
        endif()
    endif()
    foreach(_t IN ITEMS test_mysql_cross_accel test_mysql_aux_multi_connection_stress
                        test_mysql_aux_build_race test_mysql_aux_session_isolation)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state/${_t}.cpp")
            accel_add_mysql_test(${_t} tests/aux_state/${_t}.cpp)
        endif()
    endforeach()
    if(TARGET test_mysql_aux_multi_connection_stress)
        find_package(Threads REQUIRED)
        target_link_libraries(test_mysql_aux_multi_connection_stress PRIVATE Threads::Threads)
    endif()
    if(BUILD_TESTING)
        foreach(_t IN ITEMS test_mysql_cross_accel test_mysql_aux_multi_connection_stress)
            if(TARGET ${_t})
                add_test(NAME ${_t} COMMAND ${_t})
            endif()
        endforeach()
    endif()
    foreach(_t IN ITEMS benchmark_mysql_tpch_aux calibrate_mysql_tpch_aux_cost_model)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/mysql/${_t}.cpp")
            accel_add_mysql_test(${_t} benchmarks/mysql/${_t}.cpp)
        endif()
    endforeach()
endif()

# SEC EDGAR Q2 aux: differential test (maintenance matrix, random parameters).
accel_add_shared_interface_test(test_sec_edgar_q2_differential
	tests/aux_state/test_sec_edgar_q2_differential.cpp)

# SEC EDGAR Q2 aux: read-only + write-rate sweep (aux vs plain DuckDB).
add_executable(benchmark_sec_edgar_q2_sweep
	benchmarks/aux_state/benchmark_sec_edgar_q2_sweep.cpp)
target_link_libraries(benchmark_sec_edgar_q2_sweep PRIVATE accelerator_codegen duckdb_sibling)
add_executable(measure_sec_edgar_q2_state
	benchmarks/aux_state/measure_sec_edgar_q2_state.cpp)
target_link_libraries(measure_sec_edgar_q2_state PRIVATE accelerator_codegen duckdb_sibling)
add_executable(check_sec_edgar_q2_params
	benchmarks/aux_state/check_sec_edgar_q2_params.cpp)
target_link_libraries(check_sec_edgar_q2_params PRIVATE accelerator_codegen duckdb_sibling)
# SEC EDGAR Q6 aux: (fy, stmt, uom) parameter sweep against plain SQL.
add_executable(check_sec_edgar_q6_params
	benchmarks/aux_state/check_sec_edgar_q6_params.cpp)
target_link_libraries(check_sec_edgar_q6_params PRIVATE accelerator_codegen duckdb_sibling)

# ---------------------------------------------------------------------------
# SEC-EDGAR Q4 PostgreSQL differential (psql client only; no server SDK needed
# to build it, a running server with the extension installed to run it).
# ---------------------------------------------------------------------------
if(ACCELERATOR_CODEGEN_BUILD_POSTGRES AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/aux_state/test_postgres_sec_edgar_q4_differential.cpp")
    add_executable(test_postgres_sec_edgar_q4_differential
        tests/aux_state/test_postgres_sec_edgar_q4_differential.cpp)
    target_include_directories(test_postgres_sec_edgar_q4_differential PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_definitions(test_postgres_sec_edgar_q4_differential PRIVATE
        ACCEL_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# SEC-EDGAR Q2 PostgreSQL client tests (libpq only).
include(cmake/PostgresSecEdgarQ2Tests.cmake)

# Q3 parameter sweep (accel_sec_edgar_q3_aux(fy, uom)) against plain SQL on a real database.
add_executable(check_sec_edgar_q3_params
	benchmarks/aux_state/check_sec_edgar_q3_params.cpp)
target_link_libraries(check_sec_edgar_q3_params PRIVATE accelerator_codegen duckdb_sibling)
