# Registration checklist — register everywhere

New aux queries must be wired into **every** shared registration / allowlist / helper surface that existing aux queries use. Do **not** stop after creating `module.{hpp,cpp}` or a single catalog case.

## How to discover (preferred)

1. Pick a neighboring query that already passes gates (e.g. a reference accelerator from the workload).
2. `search_repo` / `grep` for that query id / `accel_*_q{N}_aux` / factory name across the tree.
3. For **each** hit that looks like shared registration, allowlisting, SQL helper dispatch, or CMake target wiring, append the same kind of entry for **this** query (append-only; do not rewrite whole files).
4. Rebuild, then verify with MCP `run_correctness` and `run_multi_connection` until both pass.

## Surfaces you must cover (outcomes, not a closed file list)

Treat these as **classes of wiring** — find the current files by searching peer queries:

| Outcome | What “done” looks like | Typical failure if missing |
|---------|------------------------|----------------------------|
| Module + factory | `AuxQueryModule` + exported `Make…Module` for this query | link / compile errors |
| Shared catalog entry | Query participates in shared catalog lookup used by differential / host bindings; **`probe_select` + `build_probe` must invalidate `base_sql`** (write benches call `CollectInvalidatingSharedProbes`) | `unsupported shared query` / bench: no invalidating probes |
| Shared-query allowlist / participation list | Query id appears wherever the repo lists which TPC-H (or SEC) queries are in the shared set | SQL entry never registered; differential / multi-conn skip or fail |
| Host SQL entrypoint (workload `engine_name`, default duckdb) | Query callable via that engine’s TF/UDF/SRF surface after rebuild (duckdb: `SELECT * FROM {{aux_function_name}}(...)`) | `Table Function … does not exist` / host-equivalent missing symbol |
| Cross-accel / multi-conn helpers | Aux SQL, reference SQL, and an invalidating writer for this query id | `unsupported … invalidating writer SQL` / multi-conn warmup fail |
| Bench arg / SQL formatters | Sweep can sample args and format base/kernel SQL for this query | `SampleTpchArgs: unsupported query_id` (or equiv.) |
| CMake targets | Smoke test, bench, calibrator targets build (auto-discovery usually enough) | missing `test_tpch_q*_aux` / bench targets |

Catalog entry alone is **not** enough. Participation lists / allowlists that drive registration loops must include this query too, or the selected engine host never registers `{{aux_function_name}}`.

## Module contract (once sources exist)

- `Id()`, `Dependencies()` — accelerator identity and watched tables
- `CreateTxnBuffer()`, `Build()`, `InterpretMutation()`, `AccumulateMutation()`, `StageMaintenance()`
- `CheckValidity()`, `Execute()`, `ExecuteFallback()`

Concurrency: read `concurrency_contract.md` — irrelevant mutations NoOp, bounded read path, per-accelerator epochs.

## Verification loop (implementation stage)

1. Build smoke + shared differential targets for this query (use this worker’s build dir).
2. MCP **`run_correctness`** → fix until `status=pass` (smoke fixtures + SF1 SQL-vs-aux).
3. MCP **`run_multi_connection`** → fix registration / TF / concurrency until `status=pass`.
4. Only then end the stage. Orchestrator still re-runs post-stage gates.
