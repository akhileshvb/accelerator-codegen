# Engine backend agent boundary

## May create/update

- `include|src/.../runtime/{engine}/` — engine-local scan helper, toy Runtime
- `include|src/.../stateful/{engine}/` — AuxBackend, StateStore, mutation/txn bridges
- Optional `src/generated/{engine}/` — extension / UDF host glue (host layer)
- Query **registries** + generic dispatch (`layer_contracts.md`, "Query registries"): the backend names no query; queries register from `src/generated/{benchmark}/q{N}/{engine}/`
- **`tests/engine_backend/test_{engine}_{layer}.cpp`** — one harness per layer in
  `layers.md` (implementation-owned)
- **`tests/engine_backend/test_{engine}_adversarial.cpp`** — validation-owned only
- CMake collectors / `ACCELERATOR_CODEGEN_BUILD_{ENGINE}` flags
- Artifacts under `pipeline/artifacts/engines/{engine}/`

`{engine}` is the workload `engine_name` (sanitized identifier).

## Must not modify

- `stateful/core|state|maintenance` (fixed substrate)
- `stateful/prims/` (shared primitives library; read-only for this pipeline. It grows only through `primitive_publish`, which aux_state query agents use; additions only, generic only)
- Per-query `src/generated/{benchmark}/q{N}/**` — layer tests must **not** depend on these, and backend files must not name a query (no per-query arrays, switches, function lists, includes)
- **Any other engine’s tree** (`runtime|stateful|{other}/`). From-scratch: substrate
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
`runtime|stateful|generated/{engine}` trees and `tests/engine_backend/test_{engine}_*.cpp`
before starting.
