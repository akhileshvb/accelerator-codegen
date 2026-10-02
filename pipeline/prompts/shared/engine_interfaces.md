# Engine interfaces (query modules vs adapters)

**Base kernels are per `engine_name`.** There is no shared portable scan IR (`DataChunk` / `ScanNativeBaseTable` → chunk consumer). Each accelerator owns that engine’s native base consume path under `src/generated/{benchmark}/q{N}/{engine}/`.

**Shared substrate is aux/txn only:** `MutationBatch`, `AuxRow`, `AuxQueryModule`, coordinator, `AuxStateStore`, maintenance, calibration. Aux modules speak those contracts on every engine; base kernels do not.

**`engine_name` is required** (CLI / `ACCEL_WORKLOAD_ENGINE_NAME` / workload YAML). There is no silent default to `duckdb` or `postgres`. Stage prompts inject an **Engine host** block for the selected engine only.

DuckDB’s `DataChunk`-shaped helpers are **host-private** under `runtime/duckdb/native_scan.hpp` (and duckdb-targeted kernels). Other engines define their own typed scan/consume helpers under `runtime/{engine}/` — never a cross-engine chunk IR.

For non-duckdb hosts, accelerator implementation may **bootstrap** missing query SQL entrypoints + differential on the existing `engine_backend` surface (`pipeline/prompts/shared/query_host_bootstrap.md`) — never a second adapter tree.

## Base kernels (query-owned, per engine)

Minimal surface per query **and** `engine_name`:

1. **`base/query.hpp`** — `*Parameters`, profile counters, `Create*Kernel`, host registration helpers
2. **`base/single-threaded/kernel.cpp`** — hot-path aggregation consuming **this engine’s** native batches / rows
3. **`base/single-threaded/package.cpp`** — bind + package registration for the **selected engine host**

Paths:

```
src/generated/{benchmark}/q{N}/{engine}/base/...
src/generated/{benchmark}/q{N}/{engine}/aux/...
```

Optional: `fused.hpp` + `fused_scan.cpp` for SEC-EDGAR or TPC-H fused paths when **this** host supports them. Fused / DuckDB-specific micro-APIs are **optional duckdb host paths**, not required for every engine.

Host registration: **the query registers itself from its own directory** with static
registrars against the backend's registries (see `generated_layout.md`, "Registration").
Backends name no query; never add a query to a backend file, catalog array, switch or list.

- **duckdb:** `q{N}/duckdb/base/single-threaded/register.cpp` (`BaseFunctionSetRegistrar`);
  `q{N}/duckdb/aux/register.cpp` (`SharedQueryRegistrar` catalog entry + `AuxQueryRegistrar`:
  table function, argument types, `MakeBinding<Scan, Module>` with the query's InitialScan)
- **other engines:** the backend's registries (`engine_backend`-owned, under `src/generated/{engine}/`
  or `runtime|stateful/{engine}/`), called from `src/generated/{bench}/q{N}/{engine}/` — do not invent
  a parallel adapter tree

Scan contract: kernels consume **engine-local** native storage / batch types for `{{engine_name}}`. Do **not** require `NativeScanSpec` → portable `DataChunk`. DuckDB may use its private `NativeScanSpec` + `DataChunk` types; other engines use their own helpers (slots, tuples, typed rebuild callbacks).

Catalog registration is per engine: the registry is `src/generated/catalog/{engine}/query_catalog.{hpp,cpp}`; the entry is registered from the query's directory.

## Aux-state

### Shared module contract (`AuxQueryModule`)

Implement in `aux/module.{hpp,cpp}` (still under the per-engine query tree):

| Method | Role |
|--------|------|
| `Id`, `Dependencies` | Accelerator identity + watched tables |
| `CreateTxnBuffer` | Per-txn incremental buffer |
| `Build` | Initial state image from **engine-local** typed scan callbacks |
| `InterpretMutation` | Relevance → `InvalidationPlan` |
| `AccumulateMutation` | Apply deltas into txn buffer |
| `StageMaintenance` | Call `services.ChooseMaintenance(...)`, materialize `StagedMaintenanceOutcome` |
| `CheckValidity` | `StateStore().CheckValidity(...)` for query window |
| `Execute` | Fast path from partition images |
| `ExecuteFallback` | Correct recompute from base tables |

Maintenance selection uses `AuxEngineServices::ChooseMaintenance` backed by `maintenance::SelectMinCostAction` and `CostModelParams` from JSON (`interference_*` fields — no separate `EstimateInterferenceCost` helper).

### Adapter responsibilities (any engine)

Engine adapters (`engine_backend`) implement substrate contracts only:

- `AuxEngineServices` / `AuxStateStore`
- Mutation → `MutationBatch`, txn lifecycle → coordinator
- Store mode: `typed_in_process` **or** `serialized_boundary` (encode/decode via `byte_stream.hpp` only at a process/SHM boundary when serialized)
- Host registration (table functions, extension entrypoints, …) for that engine
- Optional **engine-local** typed scan helpers for aux rebuild (callbacks / slots / tuples) — **not** `ScanNativeBaseTable` → portable `DataChunk`

Do **not** hard-code a second engine's file layout into query modules. When an engine adapter is absent, leave optional catalog SQL slots empty rather than inventing adapter trees mid-query.

### What query agents must not regenerate

Fixed substrate under `include/accelerator_codegen/stateful/`:

- `PartitionedStateStore`, `AuxStateStore`, `AuxRegistry` / txn coordinator
- `aux_calibration.hpp` runner
- `maintenance/action_selector.hpp`, `cost_model.hpp`

Engine adapters under `stateful/{engine}/` and `runtime/{engine}/` are owned by the **engine_backend** pipeline.

Agents own partition layouts (`src/generated/{bench}/q{N}/layout.hpp`), payload structs, mutation interpretation, incremental accumulators, maintenance staging logic (including rebuild→resync), and cost-model JSON coefficients.

## Optimization priorities

### Base kernels

1. Match production path for **this** host (fused scan for SEC; native batch / fused for TPC-H where the host supports it)
2. Filter pushdown, scan order, vector/batch fast paths, bind-time sizing
3. Coordinated bundles — not one-line micro-opts when far from the reference engine

### Aux-state

1. Pick the right **state family** (`aux_state_taxonomy.md`) before tuning coefficients
2. Partition only what the query window needs; bound read iteration to overlapping partitions
3. Irrelevant mutations → `NoOp`; avoid epoch bumps without publish effect
4. Optimize write-freq sweep (`bench_sweep_contract.md`) — primary signal is `composite_score`
5. Calibrate `interference_*` via multi-connection calibration gate, not hand-waved constants
