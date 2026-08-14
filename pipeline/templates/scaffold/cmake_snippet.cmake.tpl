# Snippet for CMakeLists.txt — query {{query_id}} ({{benchmark_name}})
# Add to accelerator_codegen sources:
#   src/generated/{{benchmark_name}}/q{{query_id}}/base/single-threaded/package.cpp
#   src/generated/{{benchmark_name}}/q{{query_id}}/base/single-threaded/kernel.cpp
# Optional (auto-included when present):
#   src/generated/{{benchmark_name}}/q{{query_id}}/base/single-threaded/fused_scan.cpp
# Add test: {{test_target}}
# Add benchmark: {{bench_target}}
