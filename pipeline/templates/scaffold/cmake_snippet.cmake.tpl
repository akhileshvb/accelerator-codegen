# Snippet for CMakeLists.txt — query {{query_id}} ({{benchmark_name}})
# Add to accelerator_codegen sources:
#   src/generated/{{source_subdir}}/q{{query_id}}_package.cpp
#   src/generated/{{source_subdir}}/q{{query_id}}_kernel.cpp
# Optional (auto-included when present):
#   src/generated/{{source_subdir}}/q{{query_id}}_fused_scan.cpp
# Add test: {{test_target}}
# Add benchmark: {{bench_target}}
