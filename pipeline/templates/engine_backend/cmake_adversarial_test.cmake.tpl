# CMake registration for engine <-> substrate ADVERSARIAL tests ({{engine_name}})
#
# Nothing to write: the glob in backends/{{engine_name}}/backend.cmake (backend.cmake.tpl)
# registers backends/{{engine_name}}/tests/test_{{engine_name}}_adversarial.cpp as target
# test_{{engine_name}}_adversarial and ctest engine_{{engine_name}}_adversarial, linked exactly
# like the layer harnesses (same SDK path via accel_backend_{{engine_name}}).
