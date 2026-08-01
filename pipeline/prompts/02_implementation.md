# Stage 2: Implementation

Implement a **fast, correctness-first** accelerator for **{{display_name}}** **Q{{query_number}}**.

## Context (compressed — full analysis on disk)

Full analysis: `{{full_analysis_path}}`

{{analysis_json}}

Reference SQL: `{{reference_sql_path}}` · Artifacts: `{{artifacts_path}}/`

## Files (patch stubs — do not rewrite from scratch)

- `{{header_path}}`, `{{kernel_path}}`, `{{package_path}}`
- `{{test_path}}`, `{{benchmark_path}}`
- Optional fused: `{{fused_scan_path}}`

Do not edit `src/register.cpp` or `CMakeLists.txt`.

{{kernel_inspiration_section}}

## Requirements

- Ordered full-result test: reference SQL vs `{{function_name}}()`
- `CreateQ{{query_number}}Kernel` + `CreateQ{{query_number}}ProfilingKernel` with phase counters (`num_aggregate_us` on fused paths)
- Benchmark: median timing, `speedup_median` CSV (templates: `{{test_template_path}}`, `{{benchmark_template_path}}`)

Build: `cmake --build build-release --target {{bench_target}} {{test_target}} -j$(nproc)` then `ctest -R {{ctest_smoke_regex}}`.

Gates run automatically: correctness → baseline → profile.

Shared docs (read_file **only if needed**): `pipeline/prompts/shared/constraints.md`, `profiling_conventions.md`, `base_kernel_optimization.md`.

{{benchmark_notes}}

{{constraints}}
