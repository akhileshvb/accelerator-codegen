# Agent boundary: aux-state query generation

## Agent may generate (query-specific)

- Partition payload structs and partition keying (`query_layouts.hpp` entries)
- `InterpretMutation` relevance / column watches
- Incremental accumulators in `CreateTxnBuffer` + `AccumulateMutation`
- `StageMaintenance` action selection inputs and staged partition updates (rebuild must **resync**, not invalidate-only)
- Initial `CostModelParams` coefficients in `config/q{N}_aux_cost_model.json`
- `aux/module.{hpp,cpp}`, optional `aux/finalize.{hpp,cpp}`
- Query-specific tests, benchmarks, and calibration binaries
- `tests/test_support/cross_accel_helpers.cpp` — append-only cases for this query (`TpchAuxSql` / `TpchReferenceSql` / `InvalidatingWriterSqlForQuery`) so `aux_cross_accel` can run
- `src/stateful/duckdb/duckdb_aux_table_functions.cpp` — **append-only** `case {N}` registering `accel_*_q{N}_aux` (required for calibrate / multi-conn / cross-accel SQL)
- `src/generated/catalog/query_catalog.cpp` — append-only module registration / dispatch
- `CMakeLists.txt` / `cmake/AuxState.cmake` when auto-discovered targets are missing (prefer globs; minimal edits only)

## Fixed substrate (do not regenerate from query pipelines)

Under `include/accelerator_codegen/stateful/`:

- **`core/`** — `AuxQueryModule`, `AuxEngineServices`, `AuxStateStore`, `AuxRegistry`, `aux_calibration.hpp`, `byte_stream.hpp`
- **`state/`** — `PartitionedStateStore`, invalidation plans, partition keys
- **`maintenance/`** — `MaintenanceAction`, `ActionCostEstimate`, `CostModelParams`, `SelectMinCostAction`

## Engine backends (`stateful/{engine}/`, `runtime/{engine}/`)

Owned by the **engine_backend** pipeline (`workload_kind: engine_backend`), not per-query agents.

- Query agents must **not** invent new engine adapters mid-query.
- Existing DuckDB/Postgres adapters are optional examples; they are regenerable from the substrate via the engine pipeline.
- Headers marked `GENERATED BACKEND` means engine-pipeline-owned.

Shared generated glue (append only from query agents):

- `src/generated/catalog/query_catalog.cpp` — module registration / dispatch
- `src/stateful/duckdb/duckdb_aux_table_functions.cpp` — DuckDB SQL `accel_*_q{N}_aux` table-function switch (**writable**; append `case {N}` only — do not rewrite the file)
- `src/generated/postgres/kernel_runner.cpp` — Postgres SQL-callable wiring (when using Postgres)

CMake scaffolding: `cmake/AuxState.cmake` (`accel_add_aux_state_query`)

## Engine-specific notes (examples)

- **DuckDB-style**: typed in-process `StateImage` payloads; no serialize on hot path
- **Postgres-style**: encode/decode at a shared-memory / process boundary only (`byte_stream.hpp`); same `AuxQueryModule` logic

See `pipeline/prompts/shared/engine_interfaces.md`, `generated_layout.md`, and `hard_guidelines.md`.

## Registration checklist

See `registration_checklist.md` in this directory.

## Runtime policy

Default: hybrid cost-based selector via `AuxEngineServices::ChooseMaintenance`.

## Concurrency

See `concurrency_contract.md`. Irrelevant mutations → `NoOp`; bounded read paths; correct `ExecuteFallback`; rebuild → resync.
