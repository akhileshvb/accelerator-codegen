# The backend build convention.
#
# backends/<engine>/backend.cmake is a backend's one entry file. The root
# CMakeLists.txt includes it when ACCELERATOR_CODEGEN_BUILD_<ENGINE> is ON
# (accel_include_backends); the rest of the build reaches the engine only through:
#
#   accel_backend_library(HOST ... CATALOG ... AUX ... BASE ...)
#       attaches the engine's host sources and per-query sources to
#       libaccelerator_codegen. Called from backend.cmake.
#   accel_backend_<engine> (INTERFACE target, defined by backend.cmake)
#       the engine SDK's include dirs and client libraries; property
#       ACCEL_BACKEND_MODULE names the module/plugin target harnesses need built first.
#   accel_add_backend_exe(<engine> <name> [CLIENT] <sources...>)
#       an executable against that interface; used by backend.cmake, its
#       legacy_targets.cmake (included right after) and the accelerators' build.cmake.
#   accel_glob_sources(<out> <repo-relative globs...>)
#       sorted sources, for module/plugin per-query code.
#
# Generated accelerator trees contribute through accelerators/<bench>/build.cmake
# (accel_include_accelerators) and the per-query sources below; no root edit adds a
# backend, a benchmark or a query.
#
# Per-query sources live in accelerators/<bench>/q<N>/<engine>/ and
# accelerators/<bench>/q<N>/common/.

# Base kernels use SIMD intrinsics guarded by __AVX512F__ / __AVX2__ with scalar
# fallbacks, so they need the target ISA enabled to take the vector paths.
# Default -march=native; -DACCEL_BASE_COPTS= (empty) builds the portable scalar fallbacks.
set(ACCEL_BASE_COPTS "-march=native" CACHE STRING "extra compile options for the base kernel sources")

# C++-only aux sources (the generated aux modules and the C++ versioned store) are
# compiled for this machine's CPU (AVX-512 on the Xeon Gold 6230).  Verus kit
# glue, substrate/verus/kit and the shared table-function/runtime plumbing keep the
# portable flags, so the Verus baseline is unchanged.  Off by default: measured on
# SF1 reads it gave no consistent gain (the module compute is a few us of a ~300 us
# read, the rest is DuckDB's own query machinery) and it makes the library
# CPU-specific.  Opt in with -DACCEL_AUX_COPTS=-march=native.
set(ACCEL_AUX_COPTS "" CACHE STRING "extra compile options for the C++ aux module sources")

function(accel_apply_base_copts)
    if(ACCEL_BASE_COPTS AND ARGN)
        separate_arguments(_opts UNIX_COMMAND "${ACCEL_BASE_COPTS}")
        set_property(SOURCE ${ARGN} APPEND PROPERTY COMPILE_OPTIONS ${_opts})
    endif()
endfunction()

function(accel_apply_aux_copts)
    if(ACCEL_AUX_COPTS AND ARGN)
        set_property(SOURCE ${ARGN} APPEND PROPERTY COMPILE_OPTIONS ${ACCEL_AUX_COPTS})
    endif()
endfunction()

function(accel_glob_sources OUT_VAR)
    set(_sources "")
    foreach(_pattern IN LISTS ARGN)
        file(GLOB _matches CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${_pattern}")
        list(APPEND _sources ${_matches})
    endforeach()
    list(SORT _sources)
    set(${OUT_VAR} ${_sources} PARENT_SCOPE)
endfunction()

# Declares ACCELERATOR_CODEGEN_BUILD_<ENGINE> for every backends/*/backend.cmake
# (ON by default for ACCEL_CODEGEN_ENGINE only; with none set, no backend is built)
# and includes the enabled ones, then the backend's legacy_targets.cmake if it has one.
# Sets ACCEL_BACKENDS to the enabled engines.
macro(accel_include_backends)
    accel_codegen_engine_name(_accel_query_engine)
    if(_accel_query_engine STREQUAL "")
        message(STATUS "accel: ACCEL_CODEGEN_ENGINE unset — skipping generated query sources")
    endif()
    file(GLOB _accel_entries CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/backends/*/backend.cmake")
    list(SORT _accel_entries)
    set(ACCEL_BACKENDS "")
    foreach(_accel_entry IN LISTS _accel_entries)
        get_filename_component(_accel_dir "${_accel_entry}" DIRECTORY)
        get_filename_component(ACCEL_BACKEND_ENGINE "${_accel_dir}" NAME)
        string(TOUPPER "${ACCEL_BACKEND_ENGINE}" _accel_upper)
        set(_accel_default OFF)
        if(ACCEL_BACKEND_ENGINE STREQUAL _accel_query_engine)
            set(_accel_default ON)
        endif()
        option(ACCELERATOR_CODEGEN_BUILD_${_accel_upper}
               "Build the ${ACCEL_BACKEND_ENGINE} backend" ${_accel_default})
        if(ACCELERATOR_CODEGEN_BUILD_${_accel_upper})
            list(APPEND ACCEL_BACKENDS "${ACCEL_BACKEND_ENGINE}")
            include("${_accel_entry}")
            include("${_accel_dir}/legacy_targets.cmake" OPTIONAL)
        endif()
    endforeach()
endmacro()

# Exposes <dir>/query_catalog.hpp as "catalog/query_catalog.hpp" through a
# build-tree prefix and compiles <dir>/*.cpp.
function(_accel_backend_catalog DIR)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/${DIR}")
    file(GLOB _cpp CONFIGURE_DEPENDS "${_src}/*.cpp")
    target_sources(accelerator_codegen PRIVATE ${_cpp})
    if(EXISTS "${_src}/query_catalog.hpp")
        set(_include_root "${CMAKE_CURRENT_BINARY_DIR}/generated_includes")
        file(MAKE_DIRECTORY "${_include_root}")
        if(NOT EXISTS "${_include_root}/catalog")
            execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink "${_src}" "${_include_root}/catalog")
        endif()
        target_include_directories(accelerator_codegen PUBLIC "${_include_root}")
    endif()
endfunction()

# Attaches a backend to libaccelerator_codegen (ACCEL_BACKEND_ENGINE is the including backend).
#   HOST     repo-relative host/runtime sources
#   CATALOG  repo-relative dir of the engine's query catalog (selected ACCEL_CODEGEN_ENGINE only)
#   AUX      globs under accelerators/*/q*/<engine>/ for the aux modules  (selected ACCEL_CODEGEN_ENGINE only)
#   BASE     globs under accelerators/*/q*/<engine>/ for the base kernels (selected ACCEL_CODEGEN_ENGINE only)
# The engine-neutral accelerators/*/q*/common/ sources come with the selected engine.
function(accel_backend_library)
    cmake_parse_arguments(BL "" "CATALOG" "HOST;AUX;BASE" ${ARGN})
    target_sources(accelerator_codegen PRIVATE ${BL_HOST})

    accel_codegen_engine_name(_engine)
    if(BL_CATALOG AND ACCEL_BACKEND_ENGINE STREQUAL _engine)
        _accel_backend_catalog("${BL_CATALOG}")
    endif()

    if(NOT ACCEL_BACKEND_ENGINE STREQUAL _engine)
        return()
    endif()
    set(_aux_globs "accelerators/*/q*/common/*.cpp")
    foreach(_pattern IN LISTS BL_AUX)
        list(APPEND _aux_globs "accelerators/*/q*/${_engine}/${_pattern}")
    endforeach()
    set(_base_globs "")
    foreach(_pattern IN LISTS BL_BASE)
        list(APPEND _base_globs "accelerators/*/q*/${_engine}/${_pattern}")
    endforeach()
    accel_glob_sources(_aux ${_aux_globs})
    accel_glob_sources(_base ${_base_globs})
    target_sources(accelerator_codegen PRIVATE ${_aux} ${_base})
    accel_apply_aux_copts(${_aux})
    accel_apply_base_copts(${_base})
endfunction()

# An executable against the engine's interface target, the library and DuckDB;
# CLIENT: a plain client of the engine's server (no library, no DuckDB).
function(accel_add_backend_exe ENGINE NAME)
    cmake_parse_arguments(BE "CLIENT" "" "" ${ARGN})
    add_executable(${NAME} ${BE_UNPARSED_ARGUMENTS})
    target_include_directories(${NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/accelerators)
    target_link_libraries(${NAME} PRIVATE accel_backend_${ENGINE})
    if(BE_CLIENT)
        return()
    endif()
    target_link_libraries(${NAME} PRIVATE accelerator_codegen duckdb_sibling)
    get_target_property(_module accel_backend_${ENGINE} ACCEL_BACKEND_MODULE)
    if(_module)
        add_dependencies(${NAME} ${_module})
    endif()
endfunction()

# Includes every accelerators/<bench>/build.cmake: the benchmark's tests, benchmarks and tools.
macro(accel_include_accelerators)
    file(GLOB _accel_builds CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/accelerators/*/build.cmake")
    list(SORT _accel_builds)
    foreach(_accel_build IN LISTS _accel_builds)
        include("${_accel_build}")
    endforeach()
endmacro()
