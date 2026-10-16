# Maintenance actions

Every accelerator offers the same actions; the cost model
(`aux_maintenance_policy.cpp`) picks the cheapest for each commit:

| Action | Effect | Cost model |
|---|---|---|
| `NoOp` | nothing relevant changed | 0 |
| `ApplyDelta` | the state is updated from the captured rows | overhead + per row |
| `RebuildPartition` | the touched partitions are rebuilt from the tables | base + per partition (an accelerator without finer partitions rebuilds its whole state, priced as `RebuildAll`, which wins a tie) |
| `RebuildAll` | the whole state is rebuilt | full rebuild |
| `InvalidateAndRebuildOnRead` | invalidated now; the next reader rebuilds it and answers from it | one full rebuild, times min(1, reads expected per write): it wins only when writes outnumber reads; offered only where readers can build the state |

Readers answer from SQL (`ExecuteFallback`) only as a last resort: when no
state can serve their snapshot (their own uncommitted writes; on PostgreSQL,
a snapshot that no certified per-backend state matches). Each engine runtime
counts how queries were answered (`aux_execute_stats.hpp`: hit, rebuilt on
read, fallback); the fuzz harnesses report it.

**Calibration** (`aux_calibration.hpp`, engine-agnostic): the policy reads
the query's cost model, `<stem>[_<engine>][_<scale>].json` in the directory
the query declares (`<bench>/q<N>/cost_model.hpp`, today
`accelerators/<bench>/q<N>/config/q<N>_aux_cost_model*.json`), found by `ResolveAuxCostModelPath`
(`aux_cost_model_path.hpp`, in this directory); each engine runtime supplies its engine name,
scale and extra directories. `CalibrateFromWrites`
runs the module's `CalibrationWrites()` (real writes of increasing size and
spread over its partitions, each with a revert and a control write on a
column it does not read) through an engine's `CalibrationEngine` hook, with
each action forced. An action's cost is the coordinator's own maintenance
time for the commit (recorded in the decision log) plus the first read's
cost over a hit, so the capture of the changed rows and the engine's commit,
the same for every action, are not charged. It fits ApplyDelta = overhead +
per row, RebuildPartition = base + per partition, RebuildAll = mean. The
engine hooks: `DuckdbCalibrationEngine` (`backends/duckdb/api/duckdb_runtime.hpp`), and the
PostgreSQL / MySQL calibrators' clients (`eval/bench/{postgres,mysql}/
calibrate_*_tpch_aux_cost_model.cpp`, over the extension's SQL controls).

**Scale.** A model may record `calibrated_scale_rows`, the rows in the
dependency tables when it was calibrated (the calibrators record it from the
engine's own row count); the engine reports the current size
(`AuxEngineServices::DataScaleRows`, or `MaintenanceRequest::data_rows_hint`;
0 = unknown). When both are known, `ratio = current / calibrated` multiplies
the `apply_delta_max_rows` cap and the rebuild costs (`full_rebuild_us`,
partition rebuild base and per-partition, the deferred rebuild); ApplyDelta's
per-row cost is not scaled, so a refresh that grows with the data stays on
ApplyDelta while it is cheaper than the scaled rebuild. When either is
unknown the model applies as calibrated (including the cap). A scale-specific
model that does not exist falls back to the nearest smaller calibrated scale
(`sf3000` uses `sf20`) before less specific names.

# Concurrency: versioned state and pending commits

All engines use one store, `VersionedAuxStore`
(`substrate/aux/state/versioned_aux_store.hpp`), which an engine only configures
through its epoch and floor providers. Epochs per engine are listed in
`substrate/aux/runtime/README.md`.

**Versions.** Each accelerator's state is a short list of versions. A version
is the accelerator's whole state (an immutable partition map; unchanged
partitions are shared between versions) and the **epoch** from which it is
current. A reader at snapshot epoch `E` sees the newest version with epoch
`<= E`. Every partition it reads comes from that one version.

- A reader epoch of `kNewestSnapshot` (the maximum epoch) means "no snapshot:
  the newest". Epoch 0 is a snapshot that sees no commit.
- A version epoch of `kAllSnapshots` (0) means "current for every snapshot";
  it replaces the history.

**One version per query.** Inside an `AuxReadScope` (opened per query by each
engine's runtime), the first read of an accelerator pins its version. Later
reads in the query reuse that version.

**Commits** (driven per accelerator by `AcceleratorRuntime`):

- `BeginCommit` marks the accelerator pending from the commit's epoch:
  - readers whose snapshot may include the commit (`E >=` that epoch, or
    newest) wait for it, without a timeout (`kWaitForPublish`; set
    `ACCEL_AUX_PENDING_WAIT_US` to bound the wait);
  - older snapshots read on.

  At most one commit per accelerator is in flight; a second `BeginCommit`
  throws.
- `FinishCommit` appends one new version (invalidations and updates applied
  together) at the commit's epoch, and ends the pending mark. A full rebuild
  (invalidate all partitions, with updates) replaces the state: partitions the
  rebuild no longer has are dropped, not left invalid.
- `AbortCommit` ends the pending mark; nothing changes.
- If maintenance fails, the runtime marks the state stale and rebuilds it;
  readers wait for the rebuild or fall back.

**Outside commits:**

- `Invalidate` makes partitions unusable for every snapshot (every kept
  version).
- `Install` replaces the state with a fresh build, current from a given
  epoch.

**Pruning.** Old versions are pruned once no open snapshot can select them.
The engine supplies the floor (the oldest open snapshot's epoch). Without a
floor provider only the newest version is kept.

**Memory cap and reclaim.** `Options::max_version_bytes` (0 = unlimited)
bounds the bytes held only by old versions (partitions no newer version
shares), sized by `Options::payload_bytes`; without that hook the cap is
inactive. Over the cap the oldest old versions are dropped
(`VersionsDroppedByCap`), and a reader whose epoch then selects no version
gets `NotVisible`, so the backend falls back. Dropped versions are destroyed
off the lock and, by default (`Options::async_reclaim`), off the commit path
on one background thread that drains at exit (`ACCEL_RECLAIM_ASYNC=0`
destroys inline).

**Locking.** Readers take a shared (writer-preferring) lock, writers an
exclusive one. A waiting reader sleeps on a condition variable. Readers get
`shared_ptr` payloads, and a module must hold the payload while using it.

**The verified Verus store.** A Verus accelerator's state can instead live
in its Verus store (`substrate/verus/store`, `substrate/aux/state/verus_aux_store.hpp`):
the same protocol (versions from engine epochs, a pending commit readers
wait for, pruning to the floor) behind one verified reader-writer lock, with
a proof that every kept version's state answers for the database at that
version. Differences from `VersionedAuxStore`:

- the module prepares each next version from the store's newest version, not
  from a payload the host passes back;
- one commit at a time (`BeginCommit` while one is in flight throws, as
  here); an install whose base is no longer the newest leaves one invalid
  version (`StaleInstalls`, should stay 0);
- a waiting reader spins with backoff (no condition variable); no
  `AuxReadScope` pinning (a module reads its state once per query).

`ACCEL_AUX_STORE=verus` selects it for accelerators that have one (the Verus
pipeline's kits), on DuckDB; anything else keeps the C++ store. Postgres
and MySQL use the C++ store.

# Calibration

`aux_calibration.hpp` holds the engine-neutral calibration runners.
`CalibrateAuxCostModel` takes timed hooks from a backend (build the initial
image, execute the auxiliary and fallback paths, apply a delta of a requested
size, rebuild one partition or everything, invalidate) and uses warmups and
median wall-clock samples. It fits the complete delta path, so the fitted
intercept goes in `apply_delta_overhead_us`, the per-row slope in
`apply_delta_per_row_us`, and `apply_delta_base_us` stays zero (the selector
would charge the intercept twice). The result is written as flat JSON that
`LoadAuxCostModelJson` reads. Calibration starts from the accelerator's
existing parameters, so thresholds survive: only the measured fields change.

A missing cost-model file falls back to default parameters. The selector's
estimate of queries before the next write comes from the engine
(`EngineSlots::ExpectedQueriesBeforeNextWrite`), or the
`ACCEL_EXPECTED_QUERIES_BEFORE_WRITE` override.

## Generic data-structure primitives

Moved to [`accelerators/prims/README.md`](../../../accelerators/prims/README.md).
