# Agent boundary: aux-state query generation

## Agent may generate (query-specific)

- Partition payload structs and partition keying (`accelerators/{bench}/q{N}/layout.hpp`)
- `InterpretMutation` relevance / column watches
- Incremental accumulators in `CreateTxnBuffer` + `AccumulateMutation`
- `StageMaintenance` action selection inputs and staged partition updates (rebuild must **resync**, not invalidate-only)
- Initial `CostModelParams` coefficients in the query's `config/` JSON, and `accelerators/{bench}/q{N}/cost_model.hpp` naming that directory (`AuxCostModelLocation`); every engine's registration passes it on
- `aux/module.{hpp,cpp}`, optional `aux/finalize.{hpp,cpp}`
- Query-specific tests, benchmarks, and calibration binaries
- **Registration from the query's own directory:** catalog entry, host SQL entry point and bindings for the workload `engine_name` through the backend's static registrars (mirror a peer query's registrar files); **append-only** entries in the shared eval helpers (cross-accel / multi-conn helper, bench-arg surface) that neighboring aux queries already use — discover paths by searching a peer query id; do not invent a partial wiring or a second adapter tree

## Fixed substrate (do not regenerate from query pipelines)

Under `substrate/aux/`:

- **`core/`** — `AuxQueryModule`, `AuxEngineServices`, `AuxStateStore`, `aux_calibration.hpp`, `byte_stream.hpp`
- **`runtime/`** — the runtime skeleton (`RuntimeHost`, `AcceleratorRuntime`, `ColumnarBatch`, `wire`, `ThreadBudget`, `engine_slots.hpp`)
- **`state/`** — `VersionedAuxStore`, invalidation plans, partition keys
- **`maintenance/`** — `MaintenanceAction`, `ActionCostEstimate`, `CostModelParams`, `SelectMinCostAction`

## Shared primitives library (`accelerators/prims/`)

Generic COW data structures and value helpers that accelerator state is built from. Query runs never write it directly (writes are denied): it grows only through `primitive_publish`. Reuse, design and publish rules: `cow_primitives.md`. Engine-exact value semantics (numeric text, division, collation) come from the backend's API (role "value semantics"), not from `prims/`.

## Engine backends (`backends/{engine}/`)

Owned by the **engine_backend** pipeline (`workload_kind: engine_backend`), not per-query agents.

- Query agents must **not** invent new engine adapters mid-query.
- Existing backends (when present) are optional few-shot only; they are regenerable from the substrate via the engine pipeline.
- Headers marked `GENERATED BACKEND` means engine-pipeline-owned.

Query runs **never write** `backends/{engine}/**` (it is denied): register this query from its own directory through the backend's registries. If the backend lacks a generic engine-level helper, log a request for its owner (one JSON line `{"benchmark", "query", "scope", "helper", "reason"}` in `pipeline/artifacts/backend_additions/{engine}.jsonl`, `scope` `"backend"` or `"harness"`, no query names in `helper`; `pipeline/prompts/shared/query_host_bootstrap.md`, Ownership) and work with what exists. CMake auto-discovery usually covers test/bench/calibrate; touch CMake only when targets are missing.

## Placement notes

The runtime hosting your module is in-process or in a shipped worker (the engine profile's `process_model` says which); the module is the same either way: typed `StateImage` payloads, no encoding of state. Only batches, arguments and result rows cross a process boundary, in substrate `wire.hpp`.

See `pipeline/prompts/shared/engine_interfaces.md`, `generated_layout.md`, and `hard_guidelines.md`.

## Registration checklist

See `registration_checklist.md` in this directory — **register everywhere**, then verify with `run_correctness` + `run_multi_connection`.

## Runtime policy

Default: hybrid cost-based selector via `AuxEngineServices::ChooseMaintenance`.

## Concurrency

See `concurrency_contract.md`. Irrelevant mutations → `NoOp`; bounded read paths; correct `ExecuteFallback`; rebuild → resync.
