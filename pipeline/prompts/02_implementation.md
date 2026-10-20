# Stage 2: Implementation

Implement a **fast, correctness-first** accelerator for **{{display_name}}** **Q{{query_number}}** on host **`{{engine_name}}`**.

{{hard_guidelines}}

{{codex_tooling}}

## Context (compressed — full analysis on disk)

Full analysis: `{{full_analysis_path}}`

{{analysis_json}}

Reference SQL: `{{reference_sql_path}}` · Artifacts: `{{artifacts_path}}/`

## Files (patch stubs — do not rewrite from scratch)

Paths are under `accelerators/.../q{{query_number}}/{{engine_name}}/`:

- `{{header_path}}`, `{{kernel_path}}`, `{{package_path}}`
- `{{test_path}}`, `{{benchmark_path}}`
- Optional fused: `{{fused_scan_path}}`
- Registration: the query's own registrar file(s) — `{{register_path}}`, `{{aux_register_path}}` (catalog entry + SQL entry point; the backends name no query)

**Engine host:** `{{engine_name}}` (**required**, no default engine). Base kernels consume **this engine’s** native batches / rows. Shared substrate is aux/txn only (`MutationBatch`, `AuxQueryModule`, …), with no portable chunk type. Registration and differential use this host.

{{backend_api}}
{{accelerator_engine_profile}}
{{engine_host_discovery}}

When the engine's harnesses lack the surfaces this workload needs, **shared** SQL entrypoints + differential + gate harnesses are created in the **`host_bootstrap`** stage (once per engine, before parallel query workers). Per-query implementation only adds this query's files (kernel, module, registrars) in its own directory. If bootstrap artifacts are missing, follow `pipeline/prompts/shared/query_host_bootstrap.md` and/or re-run `--stage host_bootstrap`. Do not invent a second adapter tree. Never edit a backend's registration code or registries: register from the query's directory. `CMakeLists.txt` may be edited for targets when auto-discovery is insufficient.

## Requirements

- Ordered full-result test: reference SQL vs accelerator on the **{{engine_name}}** host
- `CreateQ{{query_number}}Kernel` + `CreateQ{{query_number}}ProfilingKernel` with phase counters (`num_aggregate_us` on fused paths when the host uses fused APIs)
- Benchmark: median timing, `speedup_median` CSV (templates: `{{test_template_path}}`, `{{benchmark_template_path}}`) — harness matches `{{engine_name}}`; differential correctness comes first
- Base scans via **{{engine_name}}** native storage / host APIs (reuse `engine_backend` mutation/store adapters only; no portable chunk type)

**Build/smoke:** `backends/{{engine_name}}/host.sh build {{bench_target}} {{test_target}}`, then `host.sh run` the smoke tests (`ctest -R {{ctest_smoke_regex}}`). Equivalent bare form: `cmake --build build-release --target {{bench_target}} {{test_target}} -j$(nproc)` then `ctest -R {{ctest_smoke_regex}}`.

**Differential:** create/register host SQL entrypoints + `test_{{engine_name}}_tpch_differential` where absent (see query_host_bootstrap.md); the correctness gate runs that differential. Never fall back to another engine's registration.

**Exit criterion:** under Codex, call MCP `run_correctness` (or `request_gate` / the engine-selected smoke/differential above) and **do not finish until it passes**. Orchestrator still re-runs gates afterward: correctness → baseline → profile.

Shared docs (read only if needed): `pipeline/prompts/shared/generated_layout.md`, `engine_interfaces.md`, `query_host_bootstrap.md`, `constraints.md`, `hard_guidelines.md`, `profiling_conventions.md`, `base_kernel_optimization.md`.

{{benchmark_notes}}

{{constraints}}
