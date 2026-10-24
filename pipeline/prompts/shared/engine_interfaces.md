# Engine interfaces (query modules vs backends)

**Base kernels are per `engine_name`.** There is no shared portable scan IR (no cross-engine chunk or batch type). Each accelerator owns that engine’s native base consume path under `accelerators/{benchmark}/q{N}/{engine}/`.

**Shared substrate is the aux module contract plus the runtime skeleton:** `AuxQueryModule`, `AuxRow`, maintenance, calibration, and `substrate/aux/runtime/` (commit ordering, versioned state, background builds, reads at an epoch, thread budget). Aux modules speak those contracts on every engine; base kernels do not.

**The run's `engine_name` is required** (`--engine` / the pack's `engines:`; implied only when the workload lists a single engine). There is no silent default engine, and each query x engine is an independent run with its own analysis. Stage prompts inject the selected engine's **Backend API** index and **Engine profile** only.

An engine's native scan helpers are **host-private** to its backend (slot S8); they are the **scan** role of the backend API index — never a cross-engine chunk IR.

Query runs **never write** `backends/{engine}/**`, and never build a second adapter tree. A generic helper the backend lacks is a request for the engine_backend run (`pipeline/artifacts/backend_additions/{engine}.jsonl`), not a backend edit.

## Base kernels (query-owned, per engine)

Minimal surface per query **and** `engine_name`:

1. **`base/query.hpp`** — `*Parameters`, profile counters, `Create*Kernel`, host registration helpers
2. **`base/single-threaded/kernel.cpp`** — hot-path aggregation consuming **this engine’s** native batches / rows
3. **`base/single-threaded/package.cpp`** — bind + package registration for the **selected engine host**, through the **registration** role

Paths:

```
accelerators/{benchmark}/q{N}/{engine}/base/...
accelerators/{benchmark}/q{N}/{engine}/aux/...
```

Optional: `fused.hpp` + `fused_scan.cpp` for fused paths when **this** host supports them. Fused micro-APIs are optional host paths (the index and profile say whether the engine has one), not required for every engine.

Host registration: **the query registers itself from its own directory** with static
registrars against the backend's registries (see `generated_layout.md`, "Registration").
Backends name no query; never add a query to a backend file, catalog array, switch or list.

- the **registration** role of the backend API index lists the registrars (neutral `RegisterQuery` / `QueryRegistrar` and the engine's own); call them from `accelerators/{bench}/q{N}/{engine}/`

Scan contract: kernels consume **engine-local** native storage / batch types for `{{engine_name}}`. Do **not** require a portable chunk type; use the **scan** role's helpers for this engine.

Registries are per engine (the **registration** role); the entry is registered from the query's directory.

## Aux-state

### Module contract (`AuxQueryModule`)

Implement in `aux/module.{hpp,cpp}` (under the per-engine query tree):

| Method | Role |
|--------|------|
| `Id`, `Dependencies` | Accelerator identity + watched tables and columns (they set what S1 captures) |
| `CreateTxnBuffer` | Per-commit incremental buffer |
| `Build` | Initial state image from **engine-local** typed scan callbacks (S8) |
| `InterpretMutation` | Relevance -> `InvalidationPlan` |
| `AccumulateMutation` | Apply deltas into the commit's buffer |
| `StageMaintenance` | Call `services.ChooseMaintenance(...)`, materialize `StagedMaintenanceOutcome` |
| `CheckValidity` | `StateStore().CheckValidity(...)` for the query window |
| `Execute` | Fast path from partition images, at the reader's epoch |
| `ExecuteFallback` | Not written: the default calls `services.AnswerFromSql`, the engine running the query's SQL (catalog `fallback_sql`, `$n` = n-th argument) at the reader's snapshot. A fallback never builds state. |

Everything a module may call (services, parallel helpers, types, maintenance outcomes, containers, calibration) is catalogued in `substrate_primitives.md`. Modules receive each change as a projected `core::MutationBatch` (`ToMutationBatch` of the shared `ColumnarBatch`). Maintenance selection: `services.ChooseMaintenance` (`maintenance::SelectMinCostAction`, `CostModelParams` JSON, `interference_*` fields).

**Module thread rules (C12).** All parallelism comes from the backend's one thread budget through `services`. Never `std::thread::hardware_concurrency`, a fixed thread count, a private pool, or the engine's own threads (`MaxThreads`, multi-threaded table functions); `pipeline/gates/run_thread_budget_lint.sh` flags violations.

**Parallelism lives in `Execute`, `Build` and maintenance, one cap each.** `ACCEL_AUX_READ_THREADS`, `ACCEL_AUX_BUILD_THREADS` (default: the budget) and `ACCEL_AUX_MAINT_THREADS` (default 1: serial), each at most the budget; an engine may supply them from its own settings (the **thread_knobs** role names them). `services.WorkerCount()` and `services.ParallelFor(n, fn)` report and use the cap of the current phase. Maintenance runs while the accelerator's commit order is held and publishes one version at the end, so a fork-join inside `AccumulateMutation` / `StageMaintenance` races with no reader or commit; inside it, slices never call the engine (do SQL lookups serially first, then split the computation), never write the store or shared buffers (each slice owns disjoint partitions/keys or fills its own partial) and join before returning. A module whose `Execute`, `Build` or maintenance does work proportional to the data (many partitions, groups, entries, scan rows) splits it with `core::ParallelReduce` / `ParallelSlices` (`substrate/aux/core/parallel_slices.hpp`): contiguous slices of at least a fixed `grain`, so a small read is one slice on the calling thread and `WorkerCount() == 1` is the serial code. Each slice fills its own partial; merge the partials in slice order so rows and floating-point sums equal a serial run at any thread count. Slices must not call `StateStore()` / `CurrentVisibility()` (take the snapshot first and borrow its images), the sink or the builder; emit rows and `builder.Write` serially. Choose the grain so a slice is worth at least ~200 us (pool dispatch costs tens of microseconds) and measure that reads that were serial stay as fast. Do not use the engine's own threads (`MaxThreads`, multi-threaded table functions) for this. A commit's accelerators are maintained and published in parallel under the same maintenance cap, so module code shares no mutable process state (statics, caches, counters) with other modules except atomics.

**Parallel builds: `core::PartitionedBuild`** (`substrate/aux/core/partitioned_build.hpp`). A `Build` that inserts every scanned row into one state does the serial work that dominates build time; instead fill one `Partial` per partition on pool threads and merge the partials in partition order (partitions = `services.WorkerCount()`, the build cap). The engine supplies the scan (`ScanPartitioned`) and the typed row view; the module supplies only the `Partial`, how a row enters it, and how two partials merge:

```cpp
using namespace accelerator_codegen::aux_state;
Partial state = core::PartitionedBuild<Partial>(
    services, [] { return Partial{}; },
    [&](std::size_t parts, auto &&sink) {  // the scan role's partitioned scan; its signature is in the index
      scan::ScanPartitioned(services, /*engine scan handle*/ scan_handle, "events",
                            {DateColumn("ts"), DecimalColumn("amount", 2), TextColumn("status")},
                            parts, sink);
    },
    [](Partial &p, const auto &row) { p.Add(row.Date(0), row.Decimal(1), row.Char(2)); },
    [](Partial &into, Partial &&from) { into.Merge(std::move(from)); });
```

Rules: `consume` touches only its `Partial` (no engine call, no store or builder, no shared mutable state: it runs on a pool thread); `merge` is associative (integers, fixed-point sums, maps and arrays of those), so the result equals the serial build at any partition count; a partial that holds a floating-point sum passes `core::kFloatBuildPartitions` as the partition count (a function of the data, not of the thread count), so the result is bit-identical at any build thread count; threads only decide how many partitions run at once. Row accessors (same names on every engine; positions are indexes into the column list): `Int(i)` int64, `Date(i)` days since 1970-01-01 int32, `Decimal(i)` value * 10^scale int64, `Double(i)`, `TextView(i)` / `Text(i)` / `Char(i)`, `IsNull(i)`. A column that can hold NULL is declared `Nullable(...)` and tested with `IsNull` before it is read (the value of a NULL cell is unspecified); other columns must be NOT NULL. Declare columns with `IntColumn`, `DateColumn`, `DecimalColumn(name, scale)`, `DoubleColumn`, `TextColumn`. Rows are typed column batches, never boxed values. Keep `Partial` cheap to create and merge (partitions of it exist at once; do not put a per-key hash map in it when a dense array or sorted vector merges linearly), and apply anything order-dependent after the merge. `ScanPartitioned` guarantees: partition p is a fixed set of rows of the build snapshot, delivered in table order, one thread per partition at a time; with one partition it is the engine's serial scan on the calling thread (`WorkerCount() == 1` is the serial build). Tables only, no filters: filter in `consume`.

### Backend contract: slots of the runtime skeleton

A `workload_kind: engine_backend` run generates **slot implementations only**. Everything else is substrate (`substrate/aux/runtime/`: `RuntimeHost`, `AcceleratorRuntime`, `SequencedApplier`, `ColumnarBatch`, `wire`, `ThreadBudget`, and the slot interface `engine_slots.hpp`; read its `README.md` first) and **frozen**: a backend never edits it, re-implements any of it, or works around it. If a slot cannot express what the engine needs, log a backend-addition request and stop.

The frozen runtime flow every backend inherits:
1. **Capture** (S1) hands each statement's `ColumnarBatch` to `RuntimeHost::OnCapture(txn, depth, batch)`; nothing is interpreted before commit.
2. **Commit order**: `PreCommit(txn)` takes each affected accelerator's place in commit order (accelerator-id order, so commits cannot deadlock) and assigns its dense sequence number; after the engine commit, `Commit(txn, epoch)` maintains the state (lookups see the database exactly as that commit left it) and publishes a version at `epoch`; `Abort(txn)` releases the place. A commit is never skipped or dropped: while the state is building or invalid it is queued and replayed.
3. **Versioned store**: versions are published per epoch; versions below the version floor (S4) are reclaimed.
4. **Reads** resolve at the reader's epoch (S3) on the calling thread, never take the commit or build locks, wait for an in-flight commit (up to the engine's ceiling) and for a build or catch-up only while it is expected sooner than the query's SQL would finish (C11), then answer `Fallback` (the backend runs `ExecuteFallback`: SQL at the reader's own snapshot).
5. **Builds** run in the background, never inside a writer's commit: open a build snapshot at epoch `b` (S5), scan it (S8), install at `b`, then catch up (queued commits with epoch <= `b` dropped, the rest applied as one combined change). A sequence gap triggers a rebuild.
6. **Thread budget**: one pool per backend (the engine's `threads` setting; `ACCEL_AUX_THREADS` overrides; default = CPUs in the affinity mask) serves builds, apply and `Execute`; three phase caps inside it (read and build default to the budget, maintenance to 1), each from `ACCEL_AUX_{READ,BUILD,MAINT}_THREADS` or the engine's own setting (`thread_knobs` in the profile) via `runtime::ResolveReadThreads` / `ResolveBuildThreads` / `ResolveMaintenanceThreads` and `ThreadBudget::SetPhaseLimits` (`substrate/aux/runtime/thread_budget.hpp`). A backend never uses `hardware_concurrency`, a fixed thread count or a private pool; a backend with several processes shares one budget across them (`TokenSource`).

Per slot, what the backend must guarantee. How each engine meets it is in the engine profile (`process_model`, `capture`, `transactions`, `locking`, `scan`); an `engine_backend` run reads only this description and its own profile, not another engine's tree.

**S1 capture.** Every change to a tracked table, from **any** connection (not only the one that registered the accelerator), as one `ColumnarBatch` per statement and table with typed columns for `RuntimeHost::CaptureColumns(table)` (re-read when `CaptureVersion()` changes). Guarantees: old row values on update and delete; changes of a rolled-back transaction or subtransaction never reach a commit (`AbortSubtransaction`, `Abort`); a change the engine cannot express as rows (TRUNCATE) is delivered as an invalidating change so the state rebuilds; no per-row heap allocation.

**Table identity.** A module's `TableId`s are private to it (two modules may number the same table differently; workloads never share a numbering). A batch carries the host's id for the engine table, and the backend registers each accelerator with its module-to-host `TableMap` (`RuntimeHost::Register(module, store, tables)`); the runtime translates before the module sees a change. Never key capture or routing on module `TableId`s.

**S2 commit epoch.** A per-accelerator number per commit that **increases in visibility order**: if a snapshot sees commit `n` it sees every earlier one. Also `PendingEpochLowerBound(txn)`: the lowest epoch of a reader that could see the pending commit (readers from there wait for it). Guarantees: assigned under the accelerator's commit order; a commit that aborts after pre-commit is released.

**S3 reader epoch.** The newest epoch a reader's snapshot can see, exact: commit `c` is visible to the reader iff `epoch(c) <= E`. Never newer than the snapshot (torn read), never older than a commit that was visible when the read began (stale read).

**S4 version floor.** `OldestReaderEpoch()`: at most every open reader's epoch (reclaiming above a live reader breaks it; lagging only costs memory), `kNoFloor` = keep only the newest.

**S5 build snapshot.** `OpenBuildSnapshot` returns a consistent read with epoch `b`: a commit is in it **iff** its epoch <= `b`. Writes continue during the build (no quiet window, no lock that stops writers). `WaitForPreRegistrationWriters` (first build only) waits for writers that began before the accelerator's capture was registered. `MaintenanceLookupEpoch(through)` tells which epoch the engine's lookups see now when maintenance runs after visibility (engines where it differs from the commit epoch; others leave the default).

**S6 placement.** Where the runtime runs and how commits and reads reach it; `EngineWorkOnHostThread()` says whether builds and applies must run on the engine-facing thread. In-process: pool threads and the writer's thread; the read path never enters an engine-facing thread. Shipped worker: one process (or thread group) per accelerator hosting a `RuntimeHost`; commits cross as `wire.hpp`-encoded changes (`OfferCommit` / `AnnounceCommit` / `Pump`), the writer waits bounded and a commit that cannot be delivered becomes a gap (the worker rebuilds); read threads call no engine API (signals blocked, no engine headers in those files); worker restart is safe (the next build includes everything).

**S7 entry points.** One SQL-visible function per registered query: decode the arguments into `QueryArguments`, call `AcceleratorRuntime::Read(ReadOptions{epoch = S3, max_wait_us}, args, sink)`, emit the rows; on `Fallback` run `ExecuteFallback` at the reader's snapshot (SQL, via the engine's `AnswerFromSql`; report its duration with `NoteSqlFallbackUs`). The backend names no query: queries register from their own directories.

**S8 engine scans.** `RunBuild(accelerator, snapshot, builder)` runs the module's `Build` through the engine's native typed scans **inside the build snapshot**, partitioned and chunked, parallel on the thread budget, and calls `builder.Finish()`. No shared scan format.

Also supplied: `CostModel(accelerator)` (cost model resolved by engine and scale factor, nearest smaller scale when the exact one is absent), `ExpectedQueriesBeforeNextWrite`, and `TableRows(table)` (cheap row count from engine statistics, 64-bit; the runtime sums it per accelerator as `DataScaleRows`, which scales the ApplyDelta cap and rebuild costs against the model's `calibrated_scale_rows`; left at 0 the cap stays at its SF1 value and refresh-sized refreshes rebuild). Store: `VersionedAuxStore` by default (substrate); a backend never writes a store or a coordinator.

#### Backend scale expectations

- **Parallel native scans** (S8) across the thread budget: `ScanPartitioned` with the typed column specs and row view of "Parallel builds" (the module-facing surface; implement it as specified, its names are what modules call); accelerators build in parallel.
- **Phase caps and table sizes**: the three phase caps (S6/thread budget) and `TableRows` are wired from the engine; a cap the engine cannot express still honours the env override.
- **Capture once** (S1): one `ColumnarBatch` per statement per table (union of watched columns), shared by reference across accelerators.
- **No 32-bit counts or offsets** on rows, batches, images or partitions; partition keys 64-bit; decimal cells 128-bit.
- **No per-row heap allocation on capture**: typed columns, no `std::string` / `shared_ptr` / node per row.
- **Correct under concurrency** (gates `slot_*`, `pipeline/gates/README_runtime_gates.md`): no torn or stale read, writes from a second connection captured, a rebuild under continuous writes becomes synced without a quiet window, a long apply never blocks reads, hits under continuous writes.

Do **not** hard-code a second engine's file layout into query modules. When an engine adapter is absent, leave optional catalog SQL slots empty rather than inventing adapter trees mid-query.

### What query agents must not regenerate

Fixed substrate under `substrate/aux/`: `core/`, `state/` (`VersionedAuxStore`), `maintenance/`, `runtime/` (the skeleton), `aux_calibration.hpp` runner. Engine backends under `backends/{engine}/` belong to the **engine_backend** pipeline; query runs never write them.

Each query declares where its cost-model JSON lives in `accelerators/{bench}/q{N}/cost_model.hpp` (`AuxCostModelLocation`) and passes it to every engine's registration .

Agents own partition layouts (`accelerators/{bench}/q{N}/layout.hpp`), payload structs, mutation interpretation, incremental accumulators, maintenance staging logic (including rebuild->resync), and cost-model JSON coefficients.

## Optimization priorities

### Base kernels

1. Match production path for **this** host (fused scan or native batch, whichever the host supports)
2. Filter pushdown, scan order, vector/batch fast paths, bind-time sizing
3. Coordinated bundles — not one-line micro-opts when far from the reference engine

### Aux-state

1. Pick the right **state family** (`aux_state_taxonomy.md`) before tuning coefficients
2. Partition only what the query window needs; bound read iteration to overlapping partitions
3. Irrelevant mutations → `NoOp`; avoid epoch bumps without publish effect
4. Optimize write-freq sweep (`bench_sweep_contract.md`) — primary signal is `composite_score`
5. Calibrate `interference_*` via multi-connection calibration gate, not hand-waved constants
