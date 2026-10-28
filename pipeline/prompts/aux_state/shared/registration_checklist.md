# Registration checklist: register everywhere

A new aux query is wired into every registration and helper surface that existing aux queries use. Creating `module.{hpp,cpp}` or one catalog entry is not enough. Registration happens from the query's own directory through static registrars (`generated_layout.md`, "Registration"). Backends are never edited: a missing backend registry or helper is logged as a backend-addition request (format in `generated_layout.md`).

## Discover

1. Pick a neighboring query that passes the gates.
2. `rg` its query id, `accel_*_q{N}_aux` and factory name across the tree.
3. For each hit in the peer's own directory (registrars, `cost_model.hpp`, SQL-function fragment) add the same for this query under `accelerators/<bench>/q{N}/`. For hits in shared eval helpers (cross-accel helpers, bench arg variants under `eval/`) append the same kind of entry (append-only, never rewrite a file). Hits under `backends/` are registries: do not edit.
4. Rebuild, then `run_correctness` and `run_multi_connection` until both pass.

## Surfaces (outcomes; find files via peers)

| Outcome | Done means | Missing it gives |
|---|---|---|
| Module + factory | `AuxQueryModule` with exported `Make...Module` | link/compile errors |
| Catalog entry | The query's catalog registrar (registration role, in `q{N}/<engine>/aux/`) is used by differential and host bindings; `probe_select` + `build_probe` invalidate `base_sql` (write benches collect invalidating probes) | `unsupported shared query`; bench finds no invalidating probes |
| Cost-model location | `q{N}/cost_model.hpp` declares `AuxCostModelLocation`, passed to the registration (catalog entry or runtime constructor) | default coefficients, calibrated JSON ignored |
| Host SQL entry point | The registration role's SQL-entry registrars (from this query's directory) expose `{{aux_function_name}}` on the workload `engine_name` (call syntax: profile `entry_points`) | `Table Function ... does not exist` |
| Cross-accel / multi-conn helpers | Aux SQL, reference SQL and an invalidating writer for this query id | `unsupported ... invalidating writer SQL`; warmup failures |
| Bench arg / SQL formatters | The sweep samples arguments and formats engine and accelerator SQL for this query | `unsupported query_id` |
| CMake targets | Smoke test, bench, calibrator build (auto-discovery) | missing `test_<bench>_q*_aux` / bench targets |

## Verification (implementation stage)

1. Build the smoke and shared differential targets in this worker's build dir.
2. MCP `run_correctness` until `status=pass` (smoke fixtures plus iteration-scale SQL-vs-aux).
3. MCP `run_multi_connection` until `status=pass`.
4. Only then end the stage; the orchestrator re-runs the post-stage gates.
