# Notes for CMake — query {{query_id}} ({{benchmark_name}}, {{engine_name}})
# cmake/GeneratedSources.cmake globs every query's sources; nothing to add for:
#   accelerators/{{benchmark_name}}/q{{query_id}}/{{engine_name}}/base/single-threaded/*.cpp
#     (package.cpp, kernel.cpp, register.cpp; fused_scan.cpp when present)
#   accelerators/{{benchmark_name}}/q{{query_id}}/{{engine_name}}/base/parallel/*.cpp  (ACCEL_ENABLE_PARALLEL_KERNELS)
#   accelerators/{{benchmark_name}}/q{{query_id}}/{{engine_name}}/aux/*.cpp
# Header-only, included by the registration of every engine:
#   accelerators/{{benchmark_name}}/q{{query_id}}/cost_model.hpp
# Test: {{test_target}}       (accelerators/{{benchmark_name}}/q{{query_id}}/tests/{{test_target}}.cpp)
# Benchmark: {{bench_target}} (accelerators/{{benchmark_name}}/q{{query_id}}/bench/benchmark_q{{query_id}}.cpp)
