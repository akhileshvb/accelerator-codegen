# Agent boundary: aux-state query generation

## Agent may generate (query-specific)

- Partition payload structs and partition keying (`query_layouts.hpp` entries)
- `InterpretMutation` relevance / column watches
- Incremental accumulators in `CreateTxnBuffer` + `AccumulateMutation`
- `StageMaintenance` action selection inputs and staged partition updates
- Initial `CostModelParams` coefficients in `config/q{N}_aux_cost_model.json`
- `aux/module.{hpp,cpp}`, optional `aux/finalize.{hpp,cpp}`
- Query-specific tests, benchmarks, and calibration binaries
- `CMakeLists.txt` / `cmake/AuxState.cmake` when auto-discovered targets are missing (prefer globs; minimal edits only)

## Hand-written substrate (stable — do not regenerate)

Under `include/accelerator_codegen/stateful/`:

- **`core/`** — `AuxQueryModule`, `AuxEngineServices`, `AuxStateStore`, `AuxRegistry`, `aux_calibration.hpp`, `byte_stream.hpp`
- **`state/`** — `PartitionedStateStore`, invalidation plans, partition keys
- **`maintenance/`** — `MaintenanceAction`, `ActionCostEstimate`, `CostModelParams`, `SelectMinCostAction`
- **`duckdb/`** — `DuckDBAuxBackend`, `DuckDBStateStore` (typed in-process payloads)
- **`postgres/`** — `PostgresAuxBackend`, `PostgresStateStore` (byte-image serialization boundary)

Shared generated glue (append only):

- `src/generated/catalog/query_catalog.cpp` — module registration / dispatch
- `src/generated/postgres/kernel_runner.cpp` — Postgres SQL-callable wiring

CMake scaffolding: `cmake/AuxState.cmake` (`accel_add_aux_state_query`)

## Engine-specific notes

- **DuckDB**: no serialize/deserialize on the hot path; typed `StateImage` payloads in-process
- **Postgres**: encode/decode partition bytes at the shared-memory boundary only (`byte_stream.hpp`); same `AuxQueryModule` logic

See `pipeline/prompts/shared/engine_interfaces.md` and `generated_layout.md`.

## Registration checklist

See `registration_checklist.md` in this directory.

## Runtime policy

See `runtime_policy.md` in this directory. Default: hybrid cost-based selector via `AuxEngineServices::ChooseMaintenance`.

## Concurrency

See `concurrency_contract.md` in this directory. Irrelevant mutations → `NoOp`; bounded read paths; correct `ExecuteFallback`.
