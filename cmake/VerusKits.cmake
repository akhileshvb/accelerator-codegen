# Verus kits and worlds of the generated accelerator trees, linked into accelerator_codegen.
# Included by the root after the library is defined.

# Verus accelerators through the host kit (substrate/verus/kit/
# verus_kit.hpp): each accelerators/<bench>/q<N>/verus_kit/<crate>/ holds the query's host
# glue (*.cpp: the pipeline's glue.cpp, or sql2verus's gen_glue.cpp) and the
# kernel's C header; the kernel is
# ${ACCEL_VERUS_LIB_DIR}/lib<crate>_verus.a (scripts/build_verus_crate.sh).
# A glue registers itself, so nothing shared names the query.
set(ACCEL_VERUS_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build-verus"
    CACHE PATH "Directory of Verus kernel static libraries (lib<crate>_verus.a)")
# A benchmark's Verus world (scripts/gen_verus_world.py: one verified store for
# all its kits, ACCEL_AUX_STORE=verus_global) is
# accelerators/<bench>/verus_world/<world>/ (world_glue.cpp) and
# ${ACCEL_VERUS_LIB_DIR}/lib<world>_verus.a (scripts/build_verus_world.sh), an
# archive holding every kit it names: linked instead of the kits' own.
file(GLOB _verus_world_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/accelerators/*/verus_world/*")
set(_verus_world_benches)
foreach(_dir IN LISTS _verus_world_dirs)
    get_filename_component(_world "${_dir}" NAME)
    set(_lib "${ACCEL_VERUS_LIB_DIR}/lib${_world}_verus.a")
    if(IS_DIRECTORY "${_dir}" AND EXISTS "${_lib}" AND EXISTS "${_dir}/world_glue.cpp")
        string(REGEX MATCH "accelerators/([^/]+)/verus_world" _m "${_dir}")
        list(APPEND _verus_world_benches "${CMAKE_MATCH_1}")
        message(STATUS "Verus world: ${_world}")
        target_sources(accelerator_codegen PRIVATE "${_dir}/world_glue.cpp")
        set_source_files_properties("${_dir}/world_glue.cpp" PROPERTIES INCLUDE_DIRECTORIES "${_dir}")
        target_link_libraries(accelerator_codegen PRIVATE "${_lib}" pthread dl m)
    endif()
endforeach()
file(GLOB _verus_kit_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/accelerators/*/q*/verus_kit/*")
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
        string(REGEX MATCH "accelerators/([^/]+)/q" _m "${_dir}")
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
