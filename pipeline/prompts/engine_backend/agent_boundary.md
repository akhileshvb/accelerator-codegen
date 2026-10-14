# Engine backend agent boundary

## May create/update

- `backends/{engine}/engine/` and `backends/{engine}/api/` — engine-local scan helper, toy Runtime, query registries
- `backends/{engine}/state/` — AuxBackend, StateStore; `backends/{engine}/engine/` — mutation/txn bridges
- Optional `backends/{engine}/api/` — extension / UDF host glue (host layer): real entry points only
- `backends/{engine}/tests/` — gate-test UDFs and toy bridges (e.g. `extension_test_*.cpp`), built only into a test-only `<module>_test` MODULE/plugin that the layer harnesses load; never into the production MODULE/plugin
- Query **registries** + generic dispatch (`layer_contracts.md`, "Query registries"): the backend names no query; queries register from `accelerators/{benchmark}/q{N}/{engine}/`
- **`backends/{engine}/tests/test_{engine}_{layer}.cpp`** — one harness per layer in
  `layers.md` (implementation-owned)
- **`backends/{engine}/tests/test_{engine}_adversarial.cpp`** — validation-owned only
- CMake collectors / `ACCELERATOR_CODEGEN_BUILD_{ENGINE}` flags
- Artifacts under `pipeline/artifacts/engines/{engine}/`

`{engine}` is the workload `engine_name` (sanitized identifier).

This run is the only writer of `backends/{engine}/**`. Query runs never write it:
when an accelerator needs a generic engine-level helper the backend lacks, the
query run logs a request to `pipeline/artifacts/backend_additions/{engine}.jsonl`
(one JSON object per line: `benchmark`, `query`, `helper`, `reason`). Read that
log if it exists and add the requested helpers that are generic (no query, table
or benchmark names); note the ones you decline, and why, in your report.

## Must not modify

- `substrate/aux/{core,state,maintenance}` (fixed substrate)
- `accelerators/prims/` (shared primitives library; read-only for this pipeline. It grows only through `primitive_publish`, which aux_state query agents use; additions only, generic only)
- Per-query `accelerators/{benchmark}/q{N}/**` — layer tests must **not** depend on these, and backend files must not name a query (no per-query arrays, switches, function lists, includes)
- **Any other engine’s tree** (`backends/{other}/`). From-scratch: substrate
  + templates + this engine’s SDK hints only.

## Forbidden reference policy (hard)

Do **not** open, read, copy, or “adapt from” another engine adapter.

## Idempotence

If target files already exist and `allow_update_existing` is true: prefer **update/scaffold**
over full rewrite.

## Independence

Success must not depend on any other engine adapter being present or readable.
With `hide_foreign_engines: true`, the orchestrator quarantines foreign host trees
for the run.

## Correctness bar

Each implementation **layer** must pass `engine_build` plus that layer’s gate
(`engine_store`, `engine_aux_backend`, …). Together they cover
`acceptance_checklist.md`. Validation then adds unseen `engine_adversarial` tests.

**Clean-slate note:** if `allow_update_existing` is false, delete the target
`backends/{engine}/` tree and `backends/{engine}/tests/test_{engine}_*.cpp`
before starting.
