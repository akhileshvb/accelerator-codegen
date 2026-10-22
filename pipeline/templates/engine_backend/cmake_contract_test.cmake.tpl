# CMake registration for engine <-> substrate contract tests ({{engine_name}})
#
# Nothing to write: the glob in backends/{{engine_name}}/backend.cmake (backend.cmake.tpl)
# registers backends/{{engine_name}}/tests/test_{{engine_name}}_contract.cpp as target
# test_{{engine_name}}_contract and ctest engine_{{engine_name}}_contract.
#
# Native SDK libs and include dirs go on accel_backend_{{engine_name}} (INTERFACE target in
# backend.cmake), which every harness links.
# A harness that needs a tool the engine provides discovers it through the backend's own test
# helpers and fails closed if it is missing.
