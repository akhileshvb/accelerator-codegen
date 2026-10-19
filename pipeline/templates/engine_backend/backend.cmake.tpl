# backends/{{engine_name}}/backend.cmake: the backend's one CMake entry file.
# Convention: cmake/Backend.cmake. The root CMakeLists.txt includes this file when
# ACCELERATOR_CODEGEN_BUILD_{{ENGINE}} is ON; only files under backends/{{engine_name}}/ are named here.
# Helper .cmake / Find modules go in backends/{{engine_name}}/ and are included by path.

# Sources of libaccelerator_codegen: host, catalog, and the per-query kernels under
# accelerators/*/q*/{{engine_name}}/ (AUX, BASE: globs relative to that directory).
accel_backend_library(
    HOST
        backends/{{engine_name}}/api/{{engine_name}}_host.cpp
        backends/{{engine_name}}/engine/capture.cpp
        backends/{{engine_name}}/engine/scan.cpp
    CATALOG backends/{{engine_name}}/api/catalog   # omit without a query catalog
    AUX aux/*.cpp
    BASE base/single-threaded/*.cpp
)

# What harnesses link against: the engine SDK, and the module/plugin they need
# built first (host layer only). eval/harness/{{engine_name}}.cmake uses it via accel_add_backend_exe.
add_library(accel_backend_{{engine_name}} INTERFACE)
# target_include_directories(accel_backend_{{engine_name}} INTERFACE <SDK includes>)
# target_link_libraries(accel_backend_{{engine_name}} INTERFACE <client libraries>)

# In-process module/plugin (host layer): backend sources plus per-query module code.
# accel_glob_sources(_{{engine_name}}_query_sources "accelerators/*/q*/{{engine_name}}/<module dir>/*.cpp")
# add_library(accelerator_codegen_{{engine_name}} MODULE <backend sources> ${_{{engine_name}}_query_sources})
# target_link_libraries(accelerator_codegen_{{engine_name}} PRIVATE accelerator_codegen duckdb_sibling)
# set_target_properties(accel_backend_{{engine_name}} PROPERTIES ACCEL_BACKEND_MODULE accelerator_codegen_{{engine_name}})

# Layer, contract and adversarial harnesses: every tests/test_{{engine_name}}_<layer>.cpp is
# target test_{{engine_name}}_<layer> and ctest engine_{{engine_name}}_<layer>. Harnesses that need
# the test-only module variant get it as a dependency here.
file(GLOB _{{engine_name}}_layer_tests CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/backends/{{engine_name}}/tests/test_{{engine_name}}_*.cpp")
list(SORT _{{engine_name}}_layer_tests)
foreach(_src IN LISTS _{{engine_name}}_layer_tests)
    get_filename_component(_target "${_src}" NAME_WE)
    string(REGEX REPLACE "^test_{{engine_name}}_" "" _layer "${_target}")
    accel_add_backend_exe({{engine_name}} ${_target} "${_src}")
    if(BUILD_TESTING)
        add_test(NAME engine_{{engine_name}}_${_layer} COMMAND ${_target})
    endif()
endforeach()
