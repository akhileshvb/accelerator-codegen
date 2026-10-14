# CMake registration for engine ↔ substrate contract tests ({{engine_name}})
#
# Add near other test targets in CMakeLists.txt when ACCELERATOR_CODEGEN_BUILD_{{ENGINE}}
# (or always-on for in-tree engines):
#
# if({{build_flag_condition}})
#   add_executable(test_{{engine_name}}_contract
#       backends/{{engine_name}}/tests/test_{{engine_name}}_contract.cpp
#   )
#   target_link_libraries(test_{{engine_name}}_contract
#       PRIVATE
#           accelerator_codegen
#           # plus this engine's native SDK libs as needed
#   )
#   target_include_directories(test_{{engine_name}}_contract
#       PRIVATE
#           ${CMAKE_CURRENT_SOURCE_DIR}   # include root
#           # Postgres: ${ACCEL_PG_INCLUDEDIR_SERVER} from FindAccelPostgreSQL
#   )
#   # Postgres harnesses: #include "backends/postgres/tests/postgres_tooling.hpp"
#   # and call accel::postgres::DiscoverTools() — fail closed if !ok.
#   add_test(NAME engine_{{engine_name}}_contract COMMAND test_{{engine_name}}_contract)
# endif()
#
# build_flag_condition examples:
#   - TRUE                          (always-on engine)
#   - ACCELERATOR_CODEGEN_BUILD_{{ENGINE}}
