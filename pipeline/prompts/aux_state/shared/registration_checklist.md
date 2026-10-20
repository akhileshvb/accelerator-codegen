# Registration checklist — register everywhere

New aux queries must be wired into **every** registration / helper surface that existing aux queries use. Do **not** stop after creating `module.{hpp,cpp}` or a single catalog entry.

Registration happens **from the query's own directory** through static registrars (see `pipeline/prompts/shared/generated_layout.md`, "Registration"). Backends (`backends/{engine}/**`) name no query and are never written by a query run: if a backend registry or generic helper is missing, log a request to `pipeline/artifacts/backend_additions/{engine}.jsonl` (one JSON line: `benchmark`, `query`, `helper`, `reason`) and report it.

## How to discover (preferred)

1. Pick a neighboring query that already passes gates (e.g. a reference accelerator from the workload).
2. `rg` for that query id / `accel_*_q{N}_aux` / factory name across the tree.
3. For **each** hit in the peer's own directory (registrars, `cost_model.hpp`, SQL-function fragment), add the same for **this** query in `accelerators/{bench}/q{N}/`. For hits in shared eval helpers (cross-accel helpers, bench arg variants under `eval/`) append the same kind of entry (append-only; do not rewrite whole files). Hits under `backends/` are registries, not lists: never edit them.
4. Rebuild, then verify with MCP `run_correctness` and `run_multi_connection` until both pass.

## Surfaces you must cover (outcomes, not a closed file list)

Treat these as **classes of wiring** — find the current files by searching peer queries:

| Outcome | What “done” looks like | Typical failure if missing |
|---------|------------------------|----------------------------|
| Module + factory | `AuxQueryModule` + exported `Make…Module` for this query | link / compile errors |
| Catalog entry (registrar) | The query's own registrar (the **registration** role's catalog registrar, in the query's `q{N}/<engine>/aux/`) used by differential / host bindings; **`probe_select` + `build_probe` must invalidate `base_sql`** (write benches collect invalidating probes) | `unsupported shared query` / bench: no invalidating probes |
| Cost-model location | `accelerators/{bench}/q{N}/cost_model.hpp` declares the query's `AuxCostModelLocation`; passed to the registration (catalog entry or runtime constructor, per the index) | maintenance runs on default coefficients; calibrated JSON ignored |
| Host SQL entrypoint (workload `engine_name`) | Query callable via that engine’s SQL function surface after rebuild (call syntax: engine profile `entry_points`) | `Table Function … does not exist` / host-equivalent missing symbol |
| Cross-accel / multi-conn helpers | Aux SQL, reference SQL, and an invalidating writer for this query id | `unsupported … invalidating writer SQL` / multi-conn warmup fail |
| Bench arg / SQL formatters | Sweep can sample args and format base/kernel SQL for this query | `unsupported query_id` from the arg sampler (or equiv.) |
| CMake targets | Smoke test, bench, calibrator targets build (auto-discovery usually enough) | missing `test_<bench>_q*_aux` / bench targets |

Catalog entry alone is **not** enough: the host SQL entry point (the **registration** role's SQL-entry registrars) must be registered from this query's directory too, or the selected engine host never exposes `{{aux_function_name}}`.

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
