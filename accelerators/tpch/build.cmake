# TPC-H accelerators: per-query smoke tests, benchmarks and tools, discovered from
# accelerators/tpch/q<N>/{tests,bench}. Included by the root; DuckDB harnesses only.
if(NOT TARGET accel_backend_duckdb)
    return()
endif()

set(_tpch "${CMAKE_CURRENT_SOURCE_DIR}/accelerators/tpch")
file(GLOB _tests CONFIGURE_DEPENDS "${_tpch}/q*/tests/test_tpch_q*.cpp")
file(GLOB _benches CONFIGURE_DEPENDS "${_tpch}/q*/bench/benchmark_q*.cpp")
set(_rpath_targets)
foreach(_src IN LISTS _tests)
    if(_src MATCHES "/q([0-9]+)/tests/test_tpch_q[0-9]+\\.cpp$")
        set(_q ${CMAKE_MATCH_1})
        accel_add_backend_exe(duckdb test_tpch_q${_q} ${_src})
        list(APPEND _rpath_targets test_tpch_q${_q})
        if(BUILD_TESTING)
            add_test(NAME tpch_q${_q}_smoke_test COMMAND test_tpch_q${_q})
        endif()
    endif()
endforeach()
foreach(_src IN LISTS _benches)
    if(_src MATCHES "/q([0-9]+)/bench/benchmark_q[0-9]+\\.cpp$")
        accel_add_backend_exe(duckdb benchmark_tpch_q${CMAKE_MATCH_1} ${_src})
        list(APPEND _rpath_targets benchmark_tpch_q${CMAKE_MATCH_1})
    endif()
endforeach()
if(_rpath_targets)
    set_target_properties(${_rpath_targets} PROPERTIES
        BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
        INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}")
endif()

accel_add_backend_exe(duckdb test_tpch_q18_shared_differential accelerators/tpch/q18/tests/test_tpch_q18_shared_differential.cpp)

add_executable(benchmark_tpch_q20_phased_eval
    accelerators/tpch/q20/bench/benchmark_q20_phased_eval.cpp
)
target_link_libraries(benchmark_tpch_q20_phased_eval
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)
target_include_directories(benchmark_tpch_q20_phased_eval
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators
        ${CMAKE_CURRENT_SOURCE_DIR}
)
set_target_properties(benchmark_tpch_q20_phased_eval PROPERTIES
    BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
    INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
)
