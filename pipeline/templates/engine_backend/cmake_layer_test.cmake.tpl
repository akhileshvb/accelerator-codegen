# CMake registration for one engine_backend layer harness ({{engine_name}} / {{layer_id}})
#
# Nothing to write: the glob in backends/{{engine_name}}/backend.cmake (backend.cmake.tpl)
# registers backends/{{engine_name}}/tests/test_{{engine_name}}_{{layer_id}}.cpp as target
# test_{{engine_name}}_{{layer_id}} (linked by accel_add_backend_exe: what the backend
# needs comes from accel_backend_{{engine_name}}) and ctest engine_{{engine_name}}_{{layer_id}}.
#
# A harness needing more than that adds it in backend.cmake after the glob loop:
#   target_link_libraries(test_{{engine_name}}_{{layer_id}} PRIVATE <native SDK libs>)
#   add_dependencies(test_{{engine_name}}_{{layer_id}} <module>_test)   # harnesses that load the
#                                                                       # test-only module/plugin variant
