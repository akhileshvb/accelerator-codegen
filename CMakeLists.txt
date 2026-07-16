cmake_minimum_required(VERSION 3.20)

project(accelerator_codegen LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(FindSiblingDuckDB)

add_library(accelerator_codegen SHARED
    src/duckdb_scan_adapter.cpp
    src/register.cpp
    src/functions/accelerator_hello.cpp
    src/functions/count_lineitem.cpp
    src/functions/tpch_q6_function.cpp
    src/generated/tpch/q6_kernel.cpp
)

target_include_directories(accelerator_codegen
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(accelerator_codegen
    PUBLIC
        duckdb_sibling
)

target_compile_features(accelerator_codegen PUBLIC cxx_std_17)

add_executable(test_registration
    tests/test_registration.cpp
)

target_link_libraries(test_registration
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

add_executable(test_count_lineitem
    tests/test_count_lineitem.cpp
)

target_link_libraries(test_count_lineitem
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

add_executable(test_tpch_q6
    tests/test_tpch_q6.cpp
)

target_link_libraries(test_tpch_q6
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(test_tpch_q6 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(benchmark_tpch_q6
    benchmarks/tpch/benchmark_q6.cpp
)

target_link_libraries(benchmark_tpch_q6
    PRIVATE
        accelerator_codegen
        duckdb_sibling
)

target_include_directories(benchmark_tpch_q6 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

set_target_properties(accelerator_codegen test_registration test_count_lineitem test_tpch_q6 benchmark_tpch_q6 PROPERTIES
    BUILD_RPATH "${DUCKDB_LIBRARY_DIR}"
    INSTALL_RPATH "${DUCKDB_LIBRARY_DIR}"
)

if(BUILD_TESTING)
    add_test(NAME registration_smoke_test COMMAND test_registration)
    add_test(NAME count_lineitem_smoke_test COMMAND test_count_lineitem)
    add_test(NAME tpch_q6_smoke_test COMMAND test_tpch_q6)
    set_tests_properties(count_lineitem_smoke_test tpch_q6_smoke_test PROPERTIES RESOURCE_LOCK tpch_sf1_db)
endif()
