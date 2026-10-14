# CMake registration for engine ↔ substrate ADVERSARIAL tests ({{engine_name}})
#
# Validation stage only — add near the contract target when the engine flag is on:
#
# if({{build_flag_condition}})
#   add_executable(test_{{engine_name}}_adversarial
#       backends/{{engine_name}}/tests/test_{{engine_name}}_adversarial.cpp
#   )
#   target_link_libraries(test_{{engine_name}}_adversarial
#       PRIVATE
#           accelerator_codegen
#           # plus engine-specific libs as needed
#   )
#   target_include_directories(test_{{engine_name}}_adversarial
#       PRIVATE
#           ${CMAKE_CURRENT_SOURCE_DIR}/tests
#   )
#   add_test(NAME engine_{{engine_name}}_adversarial COMMAND test_{{engine_name}}_adversarial)
# endif()
#
# build_flag_condition examples:
#   - TRUE
#   - ACCELERATOR_CODEGEN_BUILD_{{ENGINE}}
