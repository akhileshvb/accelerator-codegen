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
dependency tables when it was calibrated; the engine reports the current size
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
(`substrate/aux/state/versioned_aux_store.hpp`); each engine store only configures it.

**Versions.** Each accelerator's state is a short list of versions. A version
is the accelerator's whole state (an immutable partition map; unchanged
partitions are shared between versions) and the **epoch** from which it is
current. A reader at snapshot epoch `E` sees the newest version with epoch
`<= E`. Every partition it reads comes from that one version.

- A reader epoch of `kNewestSnapshot` (0) means "no snapshot: the newest".
- A version epoch of `kAllSnapshots` (0) means "current for every snapshot";
  it replaces the history.

**One version per query.** Inside an `AuxReadScope` (opened per query by each
engine's runtime), the first read of an accelerator pins its version. Later
reads in the query reuse that version.

**Commits** (`AcceleratorRuntime`, per accelerator):

- **PRE_COMMIT**: `StageMaintenance` builds the next state (no store lock).
  Then `BeginCommit` marks the accelerator pending from the commit's epoch:
  - readers whose snapshot may include the commit (`E >=` that epoch, or
    newest) wait for it, without a timeout (`kWaitForPublish`; set
    `ACCEL_AUX_PENDING_WAIT_US` to bound the wait);
  - older snapshots read on.

  At most one commit per accelerator is in flight; a second `BeginCommit`
  throws.
- **COMMIT**: `FinishCommit` appends one new version (invalidations and
  updates applied together) at the commit's epoch, and ends the pending mark.
  A full rebuild (invalidate all partitions, with updates) replaces the
  state: partitions the rebuild no longer has are dropped, not left invalid.
- **ABORT**: `AbortCommit` ends it; nothing changes.
- **COMMIT throws**: the touched partitions are invalidated for every
  snapshot.

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

What each engine supplies:

| Engine | Snapshot epoch | Commit epoch | Floor |
|---|---|---|---|
| DuckDB | the reading transaction's start time (`DuckDBAuxBackend::ReaderScope`) | commit id + 1 (`AuxCommitClock`, told by the fork's `CommitObserver`) | lowest active start time |
| PostgreSQL | the statement's pinned `AuxMutationEpoch` | `AuxMutationEpoch` at COMMIT | none (per-backend store) |
| MySQL | newest | the runtime's epoch provider, if any | none |

DuckDB visibility is exact: a transaction sees commit `c` iff `c` is below
its start time, which is the condition for the version at `c + 1` to be
selected. A DuckDB build is current from the start time of a transaction
begun when the build starts.

**The verified Verus store.** A Verus accelerator's state can instead live
in its Verus store (`substrate/verus/store`, `substrate/aux/state/verus_aux_store.hpp`):
the same protocol (versions from engine epochs, a pending commit readers
wait for, pruning to the floor) behind one verified reader-writer lock, with
a proof that every kept version's state answers for the database at that
version. Differences from `VersionedAuxStore`:

- the module prepares each next version from the store's newest version
  (`q21vs_prepare_*`), not from a payload the host passes back;
- one commit at a time (`BeginCommit` while one is in flight throws, as
  here); an install whose base is no longer the newest leaves one invalid
  version (`StaleInstalls`, should stay 0);
- a waiting reader spins with backoff (no condition variable); no
  `AuxReadScope` pinning (a module reads its state once per query).

`ACCEL_AUX_STORE=verus` selects it for accelerators that have one (the Verus
pipeline's kits), on DuckDB; anything else keeps the C++ store. Postgres
and MySQL use the C++ store.

# Shared auxiliary-state calibration

The shared core carries immutable, query-owned state images as typed payloads.
Engine adapters decide store mode:

- **typed_in_process** — publish typed `StateImage` payloads directly (no
  serialize/deserialize on the hot path).
- **serialized_boundary** — store bytes at a process/SHM boundary and decode
  before exposing the same typed `StateSnapshot` to a module (`byte_stream.hpp`).

`aux_calibration.hpp` contains the engine-neutral calibration runner. A backend
supplies callbacks that perform the real operation against its own base tables
and state store:

- build the initial image;
- execute the auxiliary and fallback paths;
- apply a delta batch of a requested size;
- rebuild one partition and the complete state; and
- invalidate the state.

The runner uses warmups and median wall-clock samples. It fits the complete
shared-interface delta path, including state materialization, the maintenance
kernel, output refresh, and publication preparation. The fitted intercept is
stored in `apply_delta_overhead_us`, the per-row slope in
`apply_delta_per_row_us`, and the compatibility field
`apply_delta_base_us` is set to zero so the intercept is not charged twice by
the selector. The result is written as flat JSON consumed by
`LoadAuxCostModelJson`.

The engine adapter should seed calibration from its existing query-specific
configuration so thresholds and confidence parameters survive calibration.
Only the measured timing fields are replaced. This keeps calibration from
silently changing the query module's correctness or safety limits.

Adapters typically accept a `CostModelCallback` and/or an env var such as
`ACCELERATOR_CODEGEN_{ENGINE}_AUX_COST_MODEL` (see the active host adapter).
Missing files intentionally fall back to default parameters.

The maintenance selector also consumes the adapter's estimate of queries
before the next write (`WorkloadCallback` or an engine-specific expected-queries
env/GUC).

## Generic data-structure primitives

Moved to [`accelerators/prims/README.md`](../../../accelerators/prims/README.md).
