# Stage 2: Implementation (stateless kernel)

Implement the accelerator for **{{display_name}}** **Q{{query_number}}** on host **`{{engine_name}}`**.

## Goal

**Correct first, then fast at the machine level, at SF3000.**

1. The differential against the reference SQL passes on the **{{engine_name}}** host before anything else.
2. The kernel then uses the hardware: SIMD inner loops (AVX-512/AVX2 with a scalar fallback), cache-conscious columnar layouts, branch-free predicates, prefetch, no per-row allocation, fused passes. The profile and the analysis pick the loops. Read `pipeline/prompts/shared/low_level_execution.md` before writing them.
3. Capacities come from bind-time estimates, never SF1 literals (`scale_factor_contract.md`).

**Owns:** this query's files under `accelerators/.../q{{query_number}}/{{engine_name}}/`. **Never touches:** `backends/`, `substrate/`, `cmake/`, other queries, `accelerators/prims/`. **Judged by:** correctness, then baseline, profile and fairness gates (`constraints.md` below).

The test suite (`tests/workload/`, written by the tester and frozen) is the acceptance: run it with `quick_check`, never edit it; a test you can show is wrong goes into `{{artifacts_path}}/test_suite_change_request.md` (the test, the evidence, what is right) and the tester decides.

{{hard_guidelines}}

Pipeline files (`pipeline/`, `substrate/`, `backends/` gates and prompts, `cmake/`) are not yours: edits there are reverted and warned about in tool footers; if the task is blocked on one, call `report_blocker`.


{{fast_loop}}

## Context (compressed; full analysis on disk)

Full analysis: `{{full_analysis_path}}` · Reference SQL: `{{reference_sql_path}}` · Artifacts: `{{artifacts_path}}/`

{{analysis_json}}

## Files (the scaffold creates stubs; create any still missing)

- `{{header_path}}`, `{{kernel_path}}`, `{{package_path}}`
- `{{test_path}}`, `{{benchmark_path}}`
- Optional fused: `{{fused_scan_path}}`
- Registration: the query's own registrar file(s) `{{register_path}}`, `{{aux_register_path}}` (catalog entry and SQL entry point; backends name no query)

The kernel consumes **{{engine_name}}**'s native batches and rows. Registration and differential use this host; there is no default engine.

{{backend_api}}
{{accelerator_engine_profile}}
{{engine_host_discovery}}

If the host's harnesses lack a surface this query needs, the **`host_bootstrap`** stage creates the shared SQL entrypoints, differential and gate harnesses once per engine. Per-query work only adds this query's files in its own directory. If bootstrap artifacts are missing, follow `pipeline/prompts/shared/query_host_bootstrap.md` or re-run `--stage host_bootstrap`. Never edit a backend's registration code or registries: register from the query's directory. The root `CMakeLists.txt` and `cmake/` are never edited. The harness targets (differential, multi-connection, cross-accelerator, bench, calibrate, the aux module test) are host_bootstrap's: build them through the gate tools and never define or redirect them; the query's own extra targets (its oracle test) go in `accelerators/<bench>/build.cmake`. A missing or broken harness (target or behaviour) is a `report_blocker` with `owner="harness"` and the evidence, not something to rebuild in the query's tree: host_bootstrap fixes the harness before your next check.

## Requirements

- Ordered full-result test: reference SQL vs accelerator on the **{{engine_name}}** host.
- The kernel factories the scaffolded `{{kernel_path}}` declares (`Create…Kernel` and `Create…ProfilingKernel`), with phase counters (`num_aggregate_us` on fused paths).
- Benchmark: median timing and a `speedup_median` CSV (templates `{{test_template_path}}`, `{{benchmark_template_path}}`).
- Differential: create or register host SQL entrypoints and `{{differential_binary}}` where absent (`query_host_bootstrap.md`); never fall back to another engine's registration.

**Build and smoke:** `backends/{{engine_name}}/host.sh build {{bench_target}} {{test_target}}`, then `host.sh run` the smoke tests (`ctest -R {{ctest_smoke_regex}}`).

**Exit criterion:** call `run_correctness` (or `request_gate`) and do not finish until it passes. The orchestrator re-runs the gates afterward: correctness, baseline, profile.

Reference docs (read when needed): `pipeline/prompts/shared/generated_layout.md`, `engine_interfaces.md`, `query_host_bootstrap.md`, `profiling_conventions.md`.

{{benchmark_notes}}

{{constraints}}
