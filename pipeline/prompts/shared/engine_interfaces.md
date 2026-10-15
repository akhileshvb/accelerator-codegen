# Engine interfaces (query modules vs adapters)

**Base kernels are per `engine_name`.** There is no shared portable scan IR (`DataChunk` / `ScanNativeBaseTable` → chunk consumer). Each accelerator owns that engine’s native base consume path under `accelerators/{benchmark}/q{N}/{engine}/`.

**Shared substrate is aux/txn only:** `MutationBatch`, `AuxRow`, `AuxQueryModule`, coordinator, `AuxStateStore`, maintenance, calibration. Aux modules speak those contracts on every engine; base kernels do not.

**`engine_name` is required** (CLI / `ACCEL_WORKLOAD_ENGINE_NAME` / workload YAML). There is no silent default to `duckdb` or `postgres`. Stage prompts inject an **Engine host** block for the selected engine only.

DuckDB’s `DataChunk`-shaped helpers are **host-private** under `backends/duckdb/engine/native_scan.hpp` (and duckdb-targeted kernels). Other engines' `engine_backend` runs define their own typed scan/consume helpers under `backends/{engine}/engine/` — never a cross-engine chunk IR.

Query runs **never write** `backends/{engine}/**`. For non-duckdb hosts, accelerator implementation may **bootstrap** the eval-side differential + gate harnesses on the existing `engine_backend` surface (`pipeline/prompts/shared/query_host_bootstrap.md`) — never a second adapter tree. A generic helper the backend lacks is a request for the engine_backend run (`pipeline/artifacts/backend_additions/{engine}.jsonl`), not a backend edit.

## Base kernels (query-owned, per engine)

Minimal surface per query **and** `engine_name`:

1. **`base/query.hpp`** — `*Parameters`, profile counters, `Create*Kernel`, host registration helpers
2. **`base/single-threaded/kernel.cpp`** — hot-path aggregation consuming **this engine’s** native batches / rows
3. **`base/single-threaded/package.cpp`** — bind + package registration for the **selected engine host**

Paths:

```
accelerators/{benchmark}/q{N}/{engine}/base/...
accelerators/{benchmark}/q{N}/{engine}/aux/...
```

Optional: `fused.hpp` + `fused_scan.cpp` for SEC-EDGAR or TPC-H fused paths when **this** host supports them. Fused / DuckDB-specific micro-APIs are **optional duckdb host paths**, not required for every engine.

Host registration: **the query registers itself from its own directory** with static
registrars against the backend's registries (see `generated_layout.md`, "Registration").
Backends name no query; never add a query to a backend file, catalog array, switch or list.

- **duckdb:** `q{N}/duckdb/base/single-threaded/register.cpp` (`BaseFunctionSetRegistrar`);
  `q{N}/duckdb/aux/register.cpp` (`SharedQueryRegistrar` catalog entry + `AuxQueryRegistrar`:
  table function, argument types, `MakeBinding<Scan, Module>` with the query's InitialScan)
- **other engines:** the backend's registries (`engine_backend`-owned, under `backends/{engine}/api/`
  or `backends/{engine}/`), called from `accelerators/{bench}/q{N}/{engine}/` — do not invent
  a parallel adapter tree

Scan contract: kernels consume **engine-local** native storage / batch types for `{{engine_name}}`. Do **not** require `NativeScanSpec` → portable `DataChunk`. DuckDB may use its private `NativeScanSpec` + `DataChunk` types; other engines use their own helpers (slots, tuples, typed rebuild callbacks).

Catalog registration is per engine: the registry is `backends/{engine}/api/catalog/query_catalog.{hpp,cpp}`; the entry is registered from the query's directory.

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

Modules receive each change as a projected `core::MutationBatch` (`ToMutationBatch` of the shared `ColumnarBatch`) and take every worker count from `services.WorkerCount()`.

Maintenance selection uses `AuxEngineServices::ChooseMaintenance` backed by `maintenance::SelectMinCostAction` and `CostModelParams` from JSON (`interference_*` fields — no separate `EstimateInterferenceCost` helper).

### Adapter responsibilities (any engine)

The substrate owns the runtime flow (`substrate/aux/runtime/`, see its `README.md`): per-transaction capture, per-accelerator commit ordering, state maintenance, background builds with catch-up, reads at a snapshot epoch, version management, the thread pool. A backend (`engine_backend`) implements only the **slots** in `substrate/aux/runtime/engine_slots.hpp` and calls into `RuntimeHost` / `AcceleratorRuntime`.

#### Slots a backend implements

| Slot | Backend supplies | Directory |
|---|---|---|
| S1 capture | Every change to a tracked table, from **any** connection, delivered once per statement as a `ColumnarBatch` via `RuntimeHost::OnCapture` (DuckDB: database-level hook in the fork) | `engine/capture` |
| S2 commit epoch | Per accelerator, a dense sequence in visibility order plus the engine epoch (DuckDB commit id + 1; MySQL counter under `CommitMutex`; Postgres per-accelerator `gate_A` advisory lock assigning `n_A`) | `state/` |
| S3 reader epoch | The epoch a reader reads at (DuckDB txn start time; MySQL newest; Postgres `k_A` from published `(n_A, xid)`) | `state/` |
| S4 version floor | Oldest epoch any open reader may use | `state/` |
| S5 build snapshot | A consistent scan snapshot with epoch `b` (a commit is in it iff its epoch `<= b`) and the registration barrier `WaitForPreRegistrationWriters` | `engine/scan` |
| S6 placement | Where engine-facing work runs: in-process threads, or (Postgres) a per-accelerator shard worker whose read threads never call Postgres APIs, with an SPI main thread, over shared-memory rings | `api/` or `state/` |
| S7 SQL entry points | Decode arguments, emit rows (DuckDB table functions with `MaxThreads() > 1`) | `api/` |
| S8 engine scans | Parallel, partitioned scans over the engine's own native batches. No shared scan format | `engine/scan` |

Also: store mode (`typed_in_process` or `serialized_boundary`; `byte_stream.hpp` only at a process/SHM boundary), host registration for that engine, and the cost model / `ReadersCanFallBack` services.

#### A backend never implements

Runtime flow, commit ordering, gap handling, catch-up, build scheduling, version management, reader/build locks, thread pools. Those are substrate (`AcceleratorRuntime`, `SequencedApplier`, `ThreadBudget`). If a slot cannot express what the engine needs, log a backend-addition request; do not work around it in the backend.

#### Backend scale expectations

- **Parallel native scans** (S8): partition the base scan across the thread budget; builds scan in parallel.
- **Capture once** (S1): one `ColumnarBatch` per statement per table (union of watched columns), shared by reference across accelerators. No per-accelerator re-capture.
- **No 32-bit counts or offsets** on rows, batches, images or partitions; partition keys are 64-bit; decimal cells are 128-bit.
- **No per-row heap allocation on capture**: typed columns, no `std::string` / `shared_ptr` / node per row.
- **Correct under concurrency**: a read issued after a commit returns includes it; writes from a second connection are captured; a rebuild under continuous writes becomes synced without a quiet window; a long apply never blocks reads (gates: `pipeline/gates/README_runtime_gates.md`).

#### Thread rule (C12)

One thread budget per backend: DuckDB `threads`, Postgres GUC `accel.threads`, MySQL `accel_threads`; `ACCEL_AUX_THREADS` overrides; default is the CPU count of the `sched_getaffinity` mask. Backends and modules size all parallelism with `services.WorkerCount()` / `ThreadBudget`. Never use `std::thread::hardware_concurrency`, a fixed thread count, or a private thread pool. `pipeline/gates/run_thread_budget_lint.sh` flags violations.

Do **not** hard-code a second engine's file layout into query modules. When an engine adapter is absent, leave optional catalog SQL slots empty rather than inventing adapter trees mid-query.

### What query agents must not regenerate

Fixed substrate under `substrate/aux/`:

- `VersionedAuxStore`, `AuxStateStore`, `AuxRegistry` / txn coordinator
- `runtime/` (runtime skeleton: `RuntimeHost`, `AcceleratorRuntime`, `SequencedApplier`, `ColumnarBatch`, `ThreadBudget`, `EngineSlots`)
- `aux_calibration.hpp` runner
- `maintenance/action_selector.hpp`, `cost_model.hpp`

Engine adapters under `backends/{engine}/` are owned by the **engine_backend** pipeline; query runs never write them.

Each query declares where its cost-model JSON lives in `accelerators/{bench}/q{N}/cost_model.hpp` (`AuxCostModelLocation`) and passes it to every engine's registration (DuckDB `SharedQueryCatalogEntry::cost_model`; Postgres / MySQL `Runtime`).

Agents own partition layouts (`accelerators/{bench}/q{N}/layout.hpp`), payload structs, mutation interpretation, incremental accumulators, maintenance staging logic (including rebuild→resync), and cost-model JSON coefficients.

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
