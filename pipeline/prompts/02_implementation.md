# Stage 2: Implementation

Implement correctness-first accelerator for TPC-H **Q{{query_number}}**.

## Context

{{analysis_json}}

## Query

```sql
{{reference_sql}}
```

## Files to create

- `include/accelerator_codegen/tpch_q{{query_number}}.hpp`
- `src/generated/tpch/q{{query_number}}_kernel.cpp`
- `src/generated/tpch/q{{query_number}}_package.cpp`
- `tests/test_support/q{{query_number}}_manifest.hpp`
- `tests/test_tpch_q{{query_number}}.cpp`
- `benchmarks/tpch/benchmark_q{{query_number}}.cpp`

Do not edit `src/register.cpp` or `CMakeLists.txt` (scaffolded separately).

## Requirements

- Seeded differential tests (≥20 seeds when available), synthetic + transaction tests.
- Profiling: `CreateQ{{query_number}}Kernel` and `CreateQ{{query_number}}ProfilingKernel`.
- Fair benchmark harness matching existing `benchmark_q3.cpp` patterns.

After implementation, use `run_command` to build and run `ctest -R tpch_q{{query_number}}`.
Use `request_gate` with `correctness` when ready.

{{constraints}}

{{profiling_conventions}}
