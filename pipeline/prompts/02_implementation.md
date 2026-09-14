# Stage 2: Implementation

Implement a **fast, correctness-first** accelerator for **{{display_name}}** **Q{{query_number}}** on host **`{{engine_name}}`**.

{{hard_guidelines}}

{{codex_tooling}}

## Context (compressed — full analysis on disk)

Full analysis: `{{full_analysis_path}}`

{{analysis_json}}

Reference SQL: `{{reference_sql_path}}` · Artifacts: `{{artifacts_path}}/`

## Files (patch stubs — do not rewrite from scratch)

Paths are under `src/generated/.../q{{query_number}}/{{engine_name}}/`:

- `{{header_path}}`, `{{kernel_path}}`, `{{package_path}}`
- `{{test_path}}`, `{{benchmark_path}}`
- Optional fused: `{{fused_scan_path}}`
- Catalog: `src/generated/catalog/{{engine_name}}/query_catalog.{hpp,cpp}` when touched

**Engine host:** `{{engine_name}}` (**required** — no duckdb/postgres default). Base kernels consume **this engine’s** native batches / rows. Shared substrate is aux/txn only (`MutationBatch`, `AuxQueryModule`, …) — **not** portable `DataChunk`. Registration and differential use this host — see below.

{{engine_host_discovery}}

When `engine_name` is not `duckdb`, **shared** SQL entrypoints + differential + gate harnesses are created in the **`host_bootstrap`** stage (once per engine, before parallel query workers). Per-query implementation only appends this query’s registration + kernel. If bootstrap artifacts are missing, follow `pipeline/prompts/shared/query_host_bootstrap.md` and/or re-run `--stage host_bootstrap`. Do not invent a second adapter tree. DuckDB scaffold owns `src/runtime/duckdb/register.cpp` (do not hand-edit). `CMakeLists.txt` may be edited for targets when auto-discovery is insufficient.

{{kernel_inspiration_section}}

## Requirements

- Ordered full-result test: reference SQL vs accelerator on the **{{engine_name}}** host
- `CreateQ{{query_number}}Kernel` + `CreateQ{{query_number}}ProfilingKernel` with phase counters (`num_aggregate_us` on fused paths when the host uses fused APIs)
- Benchmark: median timing, `speedup_median` CSV (templates: `{{test_template_path}}`, `{{benchmark_template_path}}`) — harness matches `{{engine_name}}`; for non-duckdb, differential correctness comes first
- Base scans via **{{engine_name}}** native storage / host APIs (reuse `engine_backend` mutation/store adapters only; do **not** require `NativeScanSpec` → DataChunk)

**DuckDB build/smoke** (only when `engine_name=duckdb`): `cmake --build build-release --target {{bench_target}} {{test_target}} -j$(nproc)` then `ctest -R {{ctest_smoke_regex}}`.

**Non-duckdb:** create/register host SQL entrypoints + `test_{{engine_name}}_tpch_differential` (see query_host_bootstrap.md); correctness gate runs that differential. Never fall back to duckdb registration when `engine_name≠duckdb`.

**Exit criterion:** under Codex, call MCP `run_correctness` (or `request_gate` / the engine-selected smoke/differential above) and **do not finish until it passes**. Orchestrator still re-runs gates afterward: correctness → baseline → profile.

Shared docs (read only if needed): `pipeline/prompts/shared/generated_layout.md`, `engine_interfaces.md`, `query_host_bootstrap.md`, `constraints.md`, `hard_guidelines.md`, `profiling_conventions.md`, `base_kernel_optimization.md`.

{{benchmark_notes}}

{{constraints}}
