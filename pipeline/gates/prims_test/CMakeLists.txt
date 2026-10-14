# Standalone build of the primitives library's unit tests (no DBMS, no engine).
# Used by primitive_publish and pipeline/gates/run_prims_tests.sh:
#   cmake -S pipeline/gates/prims_test -B <build> -DPRIMS_REPO_ROOT=<repo>
cmake_minimum_required(VERSION 3.16)
project(prims_test LANGUAGES CXX)

if(NOT PRIMS_REPO_ROOT)
  message(FATAL_ERROR "pass -DPRIMS_REPO_ROOT=<repo root>")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
find_package(Threads REQUIRED)

add_executable(test_prims ${PRIMS_REPO_ROOT}/accelerators/prims/tests/test_prims.cpp)
target_include_directories(test_prims PRIVATE ${PRIMS_REPO_ROOT} ${PRIMS_REPO_ROOT}/accelerators)
target_link_libraries(test_prims PRIVATE Threads::Threads)

enable_testing()
add_test(NAME prims_unit_test COMMAND test_prims)
