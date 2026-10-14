# Snippet for CMakeLists.txt — query {{query_id}} ({{benchmark_name}})
# Add to accelerator_codegen sources:
#   accelerators/{{benchmark_name}}/q{{query_id}}/{{engine_name}}/base/single-threaded/package.cpp
#   accelerators/{{benchmark_name}}/q{{query_id}}/{{engine_name}}/base/single-threaded/kernel.cpp
# Optional (auto-included when present):
#   accelerators/{{benchmark_name}}/q{{query_id}}/{{engine_name}}/base/single-threaded/fused_scan.cpp
# Add test: {{test_target}}
# Add benchmark: {{bench_target}}
