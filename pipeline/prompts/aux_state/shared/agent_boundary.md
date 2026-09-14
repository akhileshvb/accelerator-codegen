# Agent boundary: aux-state query generation

## Agent may generate (query-specific)

- Partition payload structs and partition keying (`query_layouts.hpp` entries)
- `InterpretMutation` relevance / column watches
- Incremental accumulators in `CreateTxnBuffer` + `AccumulateMutation`
- `StageMaintenance` action selection inputs and staged partition updates (rebuild must **resync**, not invalidate-only)
- Initial `CostModelParams` coefficients in `config/q{N}_aux_cost_model.json`
- `aux/module.{hpp,cpp}`, optional `aux/finalize.{hpp,cpp}`
- Query-specific tests, benchmarks, and calibration binaries
- **Shared registration / helpers (append-only):** every catalog, allowlist, host SQL entrypoint for the workload `engine_name` (default duckdb), cross-accel / multi-conn helper, and bench-arg surface that neighboring aux queries already use — discover paths by searching a peer query id; do not invent a partial wiring or a second adapter tree

## Fixed substrate (do not regenerate from query pipelines)

Under `include/accelerator_codegen/stateful/`:

- **`core/`** — `AuxQueryModule`, `AuxEngineServices`, `AuxStateStore`, `AuxRegistry`, `aux_calibration.hpp`, `byte_stream.hpp`
- **`state/`** — `PartitionedStateStore`, invalidation plans, partition keys
- **`maintenance/`** — `MaintenanceAction`, `ActionCostEstimate`, `CostModelParams`, `SelectMinCostAction`

## Engine backends (`stateful/{engine}/`, `runtime/{engine}/`)

Owned by the **engine_backend** pipeline (`workload_kind: engine_backend`), not per-query agents.

- Query agents must **not** invent new engine adapters mid-query.
- Existing adapters (when present) are optional few-shot only; they are regenerable from the substrate via the engine pipeline.
- Headers marked `GENERATED BACKEND` means engine-pipeline-owned.

Shared generated glue is **append-only** from query agents (register this query everywhere peers are registered; search to find current files). Prefer minimal appends over rewrites. CMake auto-discovery usually covers test/bench/calibrate; touch CMake only when targets are missing.

## Store-mode notes (examples)

- **typed_in_process**: typed `StateImage` payloads; no serialize on hot path
- **serialized_boundary**: encode/decode only at a process/SHM boundary (`byte_stream.hpp`); same `AuxQueryModule` logic

See `pipeline/prompts/shared/engine_interfaces.md`, `generated_layout.md`, and `hard_guidelines.md`.

## Registration checklist

See `registration_checklist.md` in this directory — **register everywhere**, then verify with `run_correctness` + `run_multi_connection`.

## Runtime policy

Default: hybrid cost-based selector via `AuxEngineServices::ChooseMaintenance`.

## Concurrency

See `concurrency_contract.md`. Irrelevant mutations → `NoOp`; bounded read paths; correct `ExecuteFallback`; rebuild → resync.
