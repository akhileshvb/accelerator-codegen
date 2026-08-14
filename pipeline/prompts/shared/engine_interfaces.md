# Engine interfaces: DuckDB vs Postgres

The query module (`AuxQueryModule` for aux, `KernelInstance` + `QueryPackage` for base) is **engine-neutral**. Engine adapters own storage, visibility, and registration.

## Base kernels

### DuckDB (primary pipeline target)

Minimal surface per query:

1. **`base/query.hpp`** — `*Parameters`, profile counters, `Create*Kernel`, `Create*FunctionSet`
2. **`base/single-threaded/kernel.cpp`** — hot-path aggregation
3. **`base/single-threaded/package.cpp`** — bind + `CreateRuntimeBackedFunctionSet`

Optional: `fused.hpp` + `fused_scan.cpp` for SEC-EDGAR or TPC-H fused paths.

Registration: table functions are registered from `src/runtime/duckdb/register.cpp` (hand-written). Agents do not edit it; scaffold emits a snippet.

Scan adapter (extend when needed): `src/runtime/duckdb/duckdb_scan_adapter.cpp`.

### Postgres

Same generated `kernel.cpp` / `query.hpp` sources are reused. Glue is **shared and monolithic**:

- Append dispatch cases to `src/generated/postgres/kernel_runner.cpp`
- Append catalog entries to `src/generated/catalog/query_catalog.cpp`

Do not create per-query Postgres runner files.

## Aux-state

### Shared module contract (`AuxQueryModule`)

Implement in `aux/module.{hpp,cpp}`:

| Method | Role |
|--------|------|
| `Id`, `Dependencies` | Accelerator identity + watched tables |
| `CreateTxnBuffer` | Per-txn incremental buffer |
| `Build` | Initial state image from typed scan callbacks |
| `InterpretMutation` | Relevance → `InvalidationPlan` |
| `AccumulateMutation` | Apply deltas into txn buffer |
| `StageMaintenance` | Call `services.ChooseMaintenance(...)`, materialize `StagedMaintenanceOutcome` |
| `CheckValidity` | `StateStore().CheckValidity(...)` for query window |
| `Execute` | Fast path from partition images |
| `ExecuteFallback` | Correct recompute from base tables |

Maintenance selection uses `AuxEngineServices::ChooseMaintenance` backed by `maintenance::SelectMinCostAction` and `CostModelParams` from JSON (`interference_*` fields — no separate `EstimateInterferenceUs` helper).

### DuckDB adapter

- **Typed in-process payloads** — no serialize/deserialize on the hot path
- State store: `include/accelerator_codegen/stateful/duckdb/duckdb_state_store.hpp`
- Backend: `duckdb_aux_backend.hpp`
- Registration: module factory wired through `catalog/query_catalog.cpp` + DuckDB table function `accel_{benchmark}_q{N}_aux`
- Calibration env: `ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL`, `ACCELERATOR_CODEGEN_DUCKDB_EXPECTED_QUERIES_BEFORE_WRITE`

### Postgres adapter

- **Serialization boundary** at shared memory: byte images ↔ typed `StateSnapshot`
- Use `include/accelerator_codegen/stateful/core/byte_stream.hpp` (`ByteWriter` / `ByteReader`) **only** for encode/decode at the Postgres boundary
- State store: `include/accelerator_codegen/stateful/postgres/postgres_state_store.hpp`
- Backend: `postgres_aux_backend.hpp`
- Append SQL-callable wiring to `src/generated/postgres/kernel_runner.cpp`
- Calibration env: `ACCELERATOR_CODEGEN_POSTGRES_Q{N}_COST_MODEL`, `ACCELERATOR_CODEGEN_POSTGRES_EXPECTED_QUERIES_BEFORE_WRITE`

### What agents must not regenerate

Substrate under `include/accelerator_codegen/stateful/`:

- `PartitionedStateStore`, `AuxStateStore`, `AuxRegistry` / txn coordinator
- `aux_calibration.hpp` runner
- `maintenance/action_selector.hpp`, `cost_model.hpp`
- Engine backends (`duckdb/`, `postgres/`)

Agents own partition layouts (`query_layouts.hpp` entries), payload structs, mutation interpretation, incremental accumulators, maintenance staging logic, and cost-model JSON coefficients.

## Optimization priorities

### Base kernels

1. Match production path (fused scan for SEC; chunk or fused for TPC-H)
2. Filter pushdown, scan order, vector fast paths, bind-time sizing
3. Coordinated bundles — not one-line micro-opts when far from DuckDB

### Aux-state

1. Pick the right **state family** (`aux_state_taxonomy.md`) before tuning coefficients
2. Partition only what the query window needs; bound read iteration to overlapping partitions
3. Irrelevant mutations → `NoOp`; avoid epoch bumps without publish effect
4. Optimize write-freq sweep (`bench_sweep_contract.md`) — primary signal is `composite_score`
5. Calibrate `interference_*` via multi-connection calibration gate, not hand-waved constants
