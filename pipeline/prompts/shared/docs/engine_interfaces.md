# Engine interfaces (query modules vs backends)

Base kernels are per `engine_name`; there is no portable scan IR. The shared substrate is the aux module contract plus the runtime skeleton (`substrate/aux/runtime/`: commit ordering, versioned state, background builds, reads at an epoch, thread budget). Aux modules speak those contracts on every engine; base kernels do not.

`engine_name` is required (`--engine` / the pack's `engines:`; implied only for a single-engine workload). Each query x engine is an independent run with its own analysis. Stage prompts inject only the selected engine's Backend API index and Engine profile.

Query runs never write `backends/{engine}/**` or build a second adapter tree; a missing generic helper is a backend-addition request (`generated_layout.md`). Native scan helpers are private to the backend (the **scan** role).

Base kernels (`{engine}/base/`: `query.hpp`, `single-threaded/kernel.cpp` over this engine's native batches/rows, `package.cpp`, optional `fused.hpp` + `fused_scan.cpp`) and registration: `generated_layout.md`. Never add a query to a backend file, catalog array, switch or list.

## Aux-state module (`AuxQueryModule`)

Implemented in `aux/module.{hpp,cpp}`:

| Method | Role |
|---|---|
| `Id`, `Dependencies` | Identity and watched tables/columns (they set what S1 captures) |
| `CreateTxnBuffer` | Per-commit incremental buffer |
| `Build` | Initial state image from engine-local typed scan callbacks (S8) |
| `InterpretMutation` | Relevance to `InvalidationPlan` |
| `AccumulateMutation` | Apply deltas into the commit's buffer |
| `StageMaintenance` | `services.ChooseMaintenance(...)`, materialize `StagedMaintenanceOutcome` |
| `CheckValidity` | `StateStore().CheckValidity(...)` for the query window |
| `Execute` | Fast path from partition images at the reader's epoch |
| `ExecuteFallback` | Not written: the default calls `services.AnswerFromSql` (catalog `fallback_sql`, `$n` = n-th argument) at the reader's snapshot. A fallback never builds state. |

Callable surface: `substrate_primitives.md`. Changes arrive as a projected `core::MutationBatch` (`ToMutationBatch` of the shared `ColumnarBatch`).

### Thread rules

All parallelism comes from the backend's one thread budget through `services`. Never use `std::thread::hardware_concurrency`, a fixed thread count, a private pool or the engine's own threads (`MaxThreads`, multi-threaded table functions); `pipeline/gates/run_thread_budget_lint.sh` flags violations.

- `Execute`, `Build` and maintenance each have a cap: `ACCEL_AUX_READ_THREADS`, `ACCEL_AUX_BUILD_THREADS` (default: the budget) and `ACCEL_AUX_MAINT_THREADS` (default 1), each at most the budget; an engine may supply them from its own settings (profile `thread_knobs`). `services.WorkerCount()` / `services.ParallelFor(n, fn)` use the current phase's cap.
- Data-proportional work (many partitions, groups, entries, scan rows) is split with `core::ParallelReduce` / `ParallelSlices` (`substrate/aux/core/parallel_slices.hpp`): contiguous slices of at least a fixed `grain`, so a small read is one slice on the calling thread and `WorkerCount() == 1` is the serial code. Each slice fills its own partial; merge in slice order so rows and floating-point sums equal a serial run at any thread count. A slice is worth at least ~200 us (pool dispatch costs tens of us); check that reads that were serial stay as fast.
- Slices never call `StateStore()` / `CurrentVisibility()` (take the snapshot first, borrow its images), the sink, the builder or the engine; emit rows and `builder.Write` serially.
- Maintenance runs under the accelerator's commit order and publishes one version at the end, so a fork-join inside `AccumulateMutation` / `StageMaintenance` races with no reader or commit. Inside it: SQL lookups serially first, then split the computation; slices never write the store or shared buffers (each owns disjoint partitions/keys or fills its own partial) and join before returning.
- A commit's accelerators are maintained and published in parallel under the same cap, so modules share no mutable process state (statics, caches, counters) except atomics.

### Parallel builds: `core::PartitionedBuild`

`substrate/aux/core/partitioned_build.hpp`. A `Build` that inserts every scanned row into one state is serial work that dominates build time. Fill one `Partial` per partition on pool threads and merge in partition order (partitions = `services.WorkerCount()`, the build cap). The backend supplies the scan (`ScanPartitioned`) and typed row view; the module supplies the `Partial`, how a row enters it, how two partials merge:

```cpp
using namespace accelerator_codegen::aux_state;
Partial state = core::PartitionedBuild<Partial>(
    services, [] { return Partial{}; },
    [&](std::size_t parts, auto &&sink) {  // the scan role's partitioned scan; signature in the index
      scan::ScanPartitioned(services, /*engine scan handle*/ scan_handle, "events",
                            {DateColumn("ts"), DecimalColumn("amount", 2), TextColumn("status")},
                            parts, sink);
    },
    [](Partial &p, const auto &row) { p.Add(row.Date(0), row.Decimal(1), row.Char(2)); },
    [](Partial &into, Partial &&from) { into.Merge(std::move(from)); });
```

- `consume` touches only its `Partial` (no engine call, store, builder or shared mutable state: it runs on a pool thread). `merge` is associative (integers, fixed-point sums, maps/arrays of those), so the result equals the serial build at any partition count.
- A partial holding a floating-point sum passes `core::kFloatBuildPartitions` as the partition count (a function of the data, not the thread count) so the result is bit-identical at any build thread count; threads only decide how many partitions run at once.
- Keep `Partial` cheap to create and merge (several exist at once): no per-key hash map where a dense array or sorted vector merges linearly. Apply order-dependent steps after the merge.
- Column specs: `IntColumn`, `DateColumn`, `DecimalColumn(name, scale)`, `DoubleColumn`, `TextColumn`; a column that can hold NULL is declared `Nullable(...)` and tested with `IsNull` before reading (a NULL cell's value is unspecified); others must be NOT NULL. Row accessors (same on every engine; positions index the column list): `Int(i)` int64, `Date(i)` days since 1970-01-01 int32, `Decimal(i)` value*10^scale int64, `Double(i)`, `TextView(i)`/`Text(i)`/`Char(i)`, `IsNull(i)`. Rows are typed column batches, never boxed values.
- `ScanPartitioned` guarantees: partition p is a fixed set of rows of the build snapshot, delivered in table order, one thread per partition at a time; with one partition it is the engine's serial scan on the calling thread. Tables only, no filters: filter in `consume`.

## Backend contract: slots of the runtime skeleton

A `workload_kind: engine_backend` run generates slot implementations only. Everything else is frozen substrate (`substrate/aux/runtime/`: `RuntimeHost`, `AcceleratorRuntime`, `SequencedApplier`, `ColumnarBatch`, `wire`, `ThreadBudget`, `engine_slots.hpp`; read its `README.md` first): never edit, re-implement or work around it. If a slot cannot express what the engine needs, log a backend-addition request and stop. How an engine meets a slot is in its profile (`process_model`, `capture`, `transactions`, `locking`, `scan`).

Frozen runtime flow:

1. **Capture** (S1): each statement's `ColumnarBatch` goes to `RuntimeHost::OnCapture(txn, depth, batch)`; nothing is interpreted before commit.
2. **Commit order**: `PreCommit(txn)` takes each affected accelerator's place (accelerator-id order, so no deadlock) and assigns its dense sequence number; after the engine commit, `Commit(txn, epoch)` maintains state (lookups see the database exactly as that commit left it) and publishes a version at `epoch`; `Abort(txn)` releases the place. A commit is never skipped or dropped: while state is building or invalid it is queued and replayed. Versions below the floor (S4) are reclaimed.
3. **Reads** resolve at the reader's epoch (S3) on the calling thread, never take commit or build locks, wait for an in-flight commit (up to the engine's ceiling) and for a build or catch-up only while expected sooner than the query's SQL would finish (C11), else answer `Fallback` (the backend runs `ExecuteFallback`).
4. **Builds** run in the background, never inside a writer's commit: open a build snapshot at epoch `b` (S5), scan it (S8), install at `b`, catch up (queued commits with epoch <= `b` dropped, the rest applied as one combined change). A sequence gap triggers a rebuild.
5. **Thread budget**: one pool per backend (engine `threads` setting; `ACCEL_AUX_THREADS` overrides; default = CPUs in the affinity mask) serves builds, apply and `Execute`. Phase caps come from `ACCEL_AUX_{READ,BUILD,MAINT}_THREADS` or the engine's setting (`thread_knobs`) via `runtime::ResolveReadThreads` / `ResolveBuildThreads` / `ResolveMaintenanceThreads` and `ThreadBudget::SetPhaseLimits` (`thread_budget.hpp`); a cap the engine cannot express still honours the env override. No `hardware_concurrency`, fixed count or private pool; a multi-process backend shares one budget (`TokenSource`).

Per slot, what the backend guarantees:

- **S1 capture.** Every change to a tracked table from any connection, as one `ColumnarBatch` per statement and table (union of watched columns, shared by reference across accelerators) with typed columns for `RuntimeHost::CaptureColumns(table)` (re-read when `CaptureVersion()` changes). Old row values on update and delete. Rolled-back transactions and subtransactions never reach a commit (`AbortSubtransaction`, `Abort`). A change not expressible as rows (TRUNCATE) arrives as an invalidating change. No per-row heap allocation (`std::string` / `shared_ptr` / node per row).
- **Table identity.** Module `TableId`s are private to the module (no shared numbering across workloads). A batch carries the host's id; the backend registers each accelerator with its module-to-host `TableMap` (`RuntimeHost::Register(module, store, tables)`) and the runtime translates. Never key capture or routing on module `TableId`s.
- **S2 commit epoch.** Per accelerator, increasing in visibility order (a snapshot seeing commit `n` sees all earlier ones), assigned under the commit order, released if the commit aborts after pre-commit. `PendingEpochLowerBound(txn)`: the lowest epoch of a reader that could see the pending commit (such readers wait).
- **S3 reader epoch.** The newest epoch the snapshot can see, exact: commit `c` is visible iff `epoch(c) <= E`. Never newer than the snapshot (torn read), never older than a commit visible when the read began (stale read).
- **S4 version floor.** `OldestReaderEpoch()` is at most every open reader's epoch (reclaiming above a live reader breaks it; lagging only costs memory); `kNoFloor` keeps only the newest.
- **S5 build snapshot.** `OpenBuildSnapshot`: consistent read at epoch `b`, a commit is in it iff its epoch <= `b`. Writes continue during the build (no quiet window, no lock stopping writers). `WaitForPreRegistrationWriters` (first build only) waits for writers that began before capture was registered. `MaintenanceLookupEpoch(through)`: the epoch the engine's lookups see when maintenance runs after visibility (others keep the default).
- **S6 placement.** `EngineWorkOnHostThread()` says whether builds and applies must run on the engine-facing thread. In-process: pool threads and the writer's thread; the read path never enters an engine-facing thread. Shipped worker: one process (or thread group) per accelerator hosting a `RuntimeHost`; commits cross as `wire.hpp`-encoded changes (`OfferCommit` / `AnnounceCommit` / `Pump`), the writer waits bounded and an undeliverable commit becomes a gap (worker rebuilds); read threads call no engine API (signals blocked, no engine headers in those files); a restart is safe.
- **S7 entry points.** One SQL-visible function per registered query: decode arguments into `QueryArguments`, call `AcceleratorRuntime::Read(ReadOptions{epoch = S3, max_wait_us}, args, sink)`, emit rows; on `Fallback` run `ExecuteFallback` at the reader's snapshot (`AnswerFromSql`; report its duration with `NoteSqlFallbackUs`). The backend names no query.
- **S8 engine scans.** `RunBuild(accelerator, snapshot, builder)` runs the module's `Build` through native typed scans inside the build snapshot, partitioned, chunked, parallel on the budget (`ScanPartitioned` with the column specs and row view above, exactly as specified: modules call those names), then `builder.Finish()`. No shared scan format.

Also supplied: `CostModel(accelerator)` (resolved by engine and scale factor, nearest smaller scale if exact is absent), `ExpectedQueriesBeforeNextWrite`, `TableRows(table)` (cheap 64-bit row count from engine statistics; summed per accelerator as `DataScaleRows`, which scales the ApplyDelta cap and rebuild costs against `calibrated_scale_rows`; at 0 the cap stays at its SF1 value and refresh-sized refreshes rebuild). Store: `VersionedAuxStore` by default; a backend never writes a store or coordinator.

Also required: no 32-bit counts or offsets on rows, batches, images or partitions; 64-bit partition keys; 128-bit decimal cells; correct under concurrency (gates `slot_*`, `pipeline/gates/README_runtime_gates.md`): no torn or stale read, second-connection writes captured, a rebuild under continuous writes syncs without a quiet window, a long apply never blocks reads, hits under continuous writes.

## Optimization priorities

Base kernels: match the production path for this host (fused scan or native batch); filter pushdown, scan order, vector/batch fast paths, bind-time sizing; coordinated bundles over one-line micro-opts. Aux state: choose the family (`aux_state_taxonomy.md`) before tuning coefficients; optimize `composite_score` of the write-frequency sweep (`bench_sweep_contract.md`); calibrate `interference_*` with the multi-connection gate. Query agents never regenerate `substrate/aux/` or `backends/` (`agent_boundary.md`).
