# CMake registration for one engine_backend layer harness ({{engine_name}} / {{layer_id}})
#
# if({{build_flag_condition}})
#   add_executable(test_{{engine_name}}_{{layer_id}}
#       tests/engine_backend/test_{{engine_name}}_{{layer_id}}.cpp
#   )
#   target_link_libraries(test_{{engine_name}}_{{layer_id}}
#       PRIVATE
#           accelerator_codegen
#           # plus this engine's native SDK libs as needed
#   )
#   target_include_directories(test_{{engine_name}}_{{layer_id}}
#       PRIVATE
#           ${CMAKE_CURRENT_SOURCE_DIR}/tests
#   )
#   add_test(NAME engine_{{engine_name}}_{{layer_id}}
#            COMMAND test_{{engine_name}}_{{layer_id}})
# endif()
#
# build_flag_condition examples:
#   - TRUE
#   - ACCELERATOR_CODEGEN_BUILD_{{ENGINE}}
