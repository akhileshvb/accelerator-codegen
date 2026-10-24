# Throwaway tests, micro-benchmarks and probes an agent writes under a query's own directory get a
# build target by existing, the way module tests do:
#   accelerators/<bench>/q<N>/scratch/<name>.cpp      -> agent_<bench>_q<N>_<name>
#   accelerators/<bench>/q<N>/tests/agent_<name>.cpp  -> agent_<bench>_q<N>_<name>
# They are EXCLUDE_FROM_ALL and registered with no ctest test: quick_check(targets=[...]) builds and
# runs them; no gate judges them.

# OUT_VAR: a list of "<target>|<source>" entries, one per agent-written source under SOURCE_ROOT.
function(accel_agent_executable_entries OUT_VAR SOURCE_ROOT)
    set(_track CONFIGURE_DEPENDS)
    if(CMAKE_SCRIPT_MODE_FILE)   # cmake -P (the entries are unit-tested that way) has no build to re-configure
        set(_track "")
    endif()
    file(GLOB _agent_sources ${_track}
        "${SOURCE_ROOT}/accelerators/*/q*/scratch/*.cpp"
        "${SOURCE_ROOT}/accelerators/*/q*/tests/agent_*.cpp")
    set(_entries "")
    foreach(_src IN LISTS _agent_sources)
        if(_src MATCHES "/accelerators/([^/]+)/q([0-9]+)/(scratch|tests)/(agent_)?([^/]+)\\.cpp$")
            list(APPEND _entries "agent_${CMAKE_MATCH_1}_q${CMAKE_MATCH_2}_${CMAKE_MATCH_5}|${_src}")
        endif()
    endforeach()
    set(${OUT_VAR} "${_entries}" PARENT_SCOPE)
endfunction()

function(accel_add_agent_executables)
    accel_codegen_engine_name(_engine)
    if(_engine STREQUAL "")
        return()
    endif()
    accel_agent_executable_entries(_entries "${CMAKE_CURRENT_SOURCE_DIR}")
    foreach(_entry IN LISTS _entries)
        if(_entry MATCHES "^([^|]+)\\|(.*)$")
            set(_target "${CMAKE_MATCH_1}")
            set(_src "${CMAKE_MATCH_2}")
            accel_add_backend_exe(${_engine} ${_target} ${_src})
            set_target_properties(${_target} PROPERTIES EXCLUDE_FROM_ALL TRUE)
            get_target_property(_rpath accelerator_codegen BUILD_RPATH)
            if(_rpath)
                set_target_properties(${_target} PROPERTIES BUILD_RPATH "${_rpath}" INSTALL_RPATH "${_rpath}")
            endif()
        endif()
    endforeach()
endfunction()
