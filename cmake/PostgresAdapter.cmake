function(accel_collect_postgres_pg_sources OUT_VAR)
    # MODULE-only TUs. Do NOT recompile host stateful sources already linked
    # via libaccelerator_codegen.so (ODR / dual-singleton risk).
    # Aux modules live in libaccelerator_codegen (ACCEL_CODEGEN_ENGINE=postgres).
    # runtime.cpp + query_functions provide TPC-H SRFs over native PG SPI scans.
    # Base SRFs alias aux Execute (kernel_executor). No DataChunk kernel_runner.
    set(_sources)
    foreach(_f
        src/generated/postgres/postgres_extension.cpp
        src/generated/postgres/transition_capture.cpp
        src/generated/postgres/runtime.cpp
        src/stateful/postgres/postgres_bridge.cpp
        src/stateful/postgres/postgres_xact_callbacks.cpp
        src/runtime/postgres/query_functions.cpp
        src/runtime/postgres/kernel_executor.cpp
    )
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_f}")
            list(APPEND _sources "${_f}")
        endif()
    endforeach()
    set(${OUT_VAR} "${_sources}" PARENT_SCOPE)
endfunction()

function(accel_build_postgres_extension)
    accel_collect_postgres_pg_sources(PG_EXT_SOURCES)
    if(NOT PG_EXT_SOURCES)
        message(FATAL_ERROR
            "ACCELERATOR_CODEGEN_BUILD_POSTGRES=ON but postgres extension sources "
            "are missing (src/generated/postgres/postgres_extension.cpp, "
            "src/generated/postgres/runtime.cpp).")
    endif()
    add_library(accelerator_codegen_pg MODULE ${PG_EXT_SOURCES})
    target_include_directories(accelerator_codegen_pg
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src/generated
            ${ACCEL_PG_INCLUDEDIR}
            ${ACCEL_PG_INCLUDEDIR_SERVER}
    )
    target_link_libraries(accelerator_codegen_pg
        PRIVATE
            accelerator_codegen
            duckdb_sibling
    )
    target_compile_features(accelerator_codegen_pg PRIVATE cxx_std_17)
    target_compile_options(accelerator_codegen_pg PRIVATE -fpermissive)
    target_compile_definitions(accelerator_codegen_pg PRIVATE
        ACCELERATOR_CODEGEN_POSTGRES_EXTENSION)
    set_target_properties(accelerator_codegen_pg PROPERTIES
        PREFIX ""
        SUFFIX ".so"
        INSTALL_RPATH "$ORIGIN:$ORIGIN/../:$ORIGIN/../../:${CMAKE_BINARY_DIR}:/home/akhilvb/accelerator-codegen-duckdb/build/release/src"
        BUILD_RPATH "$ORIGIN:$ORIGIN/../:$ORIGIN/../../:${CMAKE_BINARY_DIR}:/home/akhilvb/accelerator-codegen-duckdb/build/release/src"
    )
endfunction()
