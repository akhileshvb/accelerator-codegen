# Query host bootstrap (non-duckdb accelerators)

When `engine_name` is not `duckdb`, a **one-shot `host_bootstrap` stage** runs once per engine (pack: before parallel workers; single query: before analysis). It creates shared SQL entrypoints, differential, and (when turns allow) replicated gate harnesses. Per-query stages then only add the query's own files (kernels, module, registrars) under `accelerators/{bench}/q{N}/{{engine_name}}/` — never edits to backend files.

`engine_name` is **required** (no silent duckdb/postgres default). Paths and binary names come from the workload `engine_host` block / `{{engine_name}}` (do not hardcode another engine). See also stage prompt `pipeline/prompts/host_bootstrap.md`.

Base consume is **engine-local**. Reuse `engine_backend` for mutation/store (+ optional typed rebuild scan). Do **not** require `NativeScanSpec` → portable `DataChunk`.

## Do / don’t

- **Do** build on the existing host under `backends/{{engine_name}}/` (whatever `engine_backend` produced: registries, generic dispatch, SQL/UDF entry points). It is **read-only** here: `backends/{{engine_name}}/**` belongs to the `engine_backend` run, and query runs (this stage included) never write it.
- **Do** record a request when the backend lacks a generic surface (registry, dispatch, entry point): append one JSON line `{"benchmark": ..., "query": ..., "helper": ..., "reason": ...}` to `pipeline/artifacts/backend_additions/{{engine_name}}.jsonl` (no query names in the helper) and say so in your report. The `engine_backend` run adds it; do not work around it with backend edits.
- **Do** create the eval-side gate harnesses **once** if missing (differential, multi-connection, cross-accel under `eval/`), then register this query from its own directory.
- **Do** **replicate** duckdb aux gate surfaces for this engine (same semantics, engine-native host): differential first; then multi-connection, cross-accel, bench/calibrate as needed for post-stage gates.
- **Do** register the query's catalog entry from its own directory through the registry in `backends/{{engine_name}}/api/catalog/query_catalog.{hpp,cpp}` (the backend file holds the registry only), passing the query's cost-model location from `accelerators/{bench}/q{N}/cost_model.hpp`.
- **Do not** invent a second adapter tree (no duplicate mutation / store; no portable-chunk `ScanNativeBaseTable`).
- **Do not** call duckdb `RegisterAcceleratorFunctions` / `*.duckdb` datasets for this host.

## Checklist (any non-duckdb `engine_name`)

### Host bootstrap stage (shared, once)

1. **SQL entrypoints**: check that the backend's registries cover what the conventional register surface from `engine_host` (`register_path` / `aux_register_path`) needs:
   - Base: host-native UDF / table function / SRF that can run a generated kernel via **this engine’s** native consume path.
   - Aux: host entrypoint that drives `AuxQueryModule` Execute / fallback through the existing aux backend (`MutationBatch` / store).
   - These are generic registries + dispatch in `backends/{{engine_name}}/api/` (a query's entry point, SQL function and catalog entry plug in with static registrars), written by the `engine_backend` run. Anything missing is a backend-addition request (above), not an edit here. Per-query code belongs under `.../q{N}/{{engine_name}}/`. A shared list of SQL functions (extension script, plugin UDF list) is assembled from per-query fragments, not hand-maintained.
2. **Differential harness** — if missing, add:
   - Source: `eval/tests/test_{{engine_name}}_tpch_differential.cpp` (`engine_host.differential_source`)
   - Binary / CMake target: `test_{{engine_name}}_tpch_differential` (`engine_host.differential_binary`)
   - Behavior: load TPC-H into **this** engine’s native storage, run reference SQL vs accelerator SQL for `<query_id>`, assert ordered full-result match (or the binary’s documented minimal smoke).
   - **postgres:** Docker host (`accel_pg_test`); build inside container; TPC-H in Postgres tables (`accel_tpch_test`), not `*.duckdb` as scan storage.
3. **`host_bootstrap` gate** — differential target **builds** (postgres: in-container under `/accel-build/accel/`). Pass this before expanding scope. If the post-stage gate fails, the orchestrator re-invokes `host_bootstrap` with `notes`/`log_tail`. A separate critic then checks the host is not a stub.
4. **Replicate concurrency / fairness harnesses** (same bar as duckdb, engine-native) when missing:
   - Multi-connection: ctest `test_{{engine_name}}_aux_multi_connection_stress`
   - Cross-accel: ctest `test_{{engine_name}}_cross_accel`
   - Bench / calibrate / mutation: engine-aware binaries or shared targets with `ACCEL_ENGINE_NAME` + host work dir (`ACCEL_ENGINE_WORK_DB`)

### Per-query stages (after bootstrap)

5. **Per-query registration** — add this query's registrar file(s) and SQL-function fragment in its own directory (mirror a peer query); no backend edits.
6. **Correctness / multi-conn / cross-accel / post-stage gates** must pass for this engine — keep fixing until they do (suite filtered by workload YAML, not by silently switching to duckdb).

## Exit criterion

- **`host_bootstrap` stage:** MCP `request_gate` / `host_bootstrap` passes (differential binary built).
- **Per-query implementation:** engine-selected differential **and** the same post-implementation gate suite as duckdb pass for this query. Prefer MCP `run_correctness` / `run_multi_connection` / `request_gate`.
