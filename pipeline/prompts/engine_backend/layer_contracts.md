# Engine backend layer contracts (substrate)

Engine-agnostic. Backend types live in
`accelerator_codegen::aux_state::engine::{engine}` — **never** in `core::` or
`runtime::`. Fill `pipeline/templates/engine_backend/` (keep that namespace).
Do **not** read another engine's `backends/<engine>/` tree.

If `backend_analysis.json` `harness_plan` contradicts this file or
`acceptance_checklist.md`, **follow this file**.

**Read first (every layer):** `substrate/aux/runtime/README.md`,
`engine_slots.hpp`, `runtime_host.hpp`, `accelerator_runtime.hpp`, and
`shared/engine_interfaces.md` ("Backend contract": the slot contracts and what
each must guarantee). The flow they implement is frozen: if a slot cannot
express what the engine needs, log a backend-addition request; never work
around the substrate.

**Toy module.** `backends/{engine}/tests/` owns a small `AuxQueryModule`
(per-key sum of one table; copy `SumModule` + its sink from
`eval/tests/test_aux_runtime.cpp`, drop its fake slots) used by every layer
harness.

---

## `capture` (S1)

Implements the engine's capture of writes (`engine/capture.*`).

- Deliver each statement's change on a tracked table as **one**
  `ColumnarBatch` through a delivery function the host binds to
  `RuntimeHost::OnCapture(txn, depth, batch)` (the harness binds a recorder),
  from **every**
  connection (a hook registered per connection that misses connections fails).
  Columns: exactly `RuntimeHost::CaptureColumns(table)` (the union over
  registered accelerators; empty vector = every column; none = not tracked);
  re-read when `CaptureVersion()` changes. Types per `ColumnData` (Int64,
  UInt64, ScaledDecimal, ScaledDecimal128, Double, Date, Boolean, String);
  `updated_attnums` for updates (`kOutsideProjection` for an assigned column
  that was not captured).
- Old row values on update and delete. Typed columns, **no per-row heap
  allocation** (no `std::string` / `shared_ptr` per row).
- Subtransaction depth: `CommitSubtransaction` / `AbortSubtransaction`; a
  rolled-back (sub)transaction never reaches a commit.
- A change not expressible as rows (Postgres TRUNCATE) invalidates the
  accelerators of its tables at commit (`AcceleratorRuntime::Invalidate()`;
  shipped placement: an invalidating `CommittedChange`). An engine that
  reports TRUNCATE as deletes needs nothing special.

**Harness:** the toy module registered on a `RuntimeHost` (for
`CaptureColumns`); INSERT, UPDATE, DELETE issued from two different
connections; record the batches at the delivery function and assert table id,
kind, `row_count`, `old_rows` / `new_rows` cell values (NULL, a >64-bit
decimal, a string) and `updated_attnums`; a 3-row INSERT is one batch;
register a second accelerator watching more columns -> `CaptureVersion()`
changes and later batches carry the union; ROLLBACK and ROLLBACK TO SAVEPOINT
-> no change reaches the toy state after the commit; TRUNCATE (or the
engine's non-row change) -> the state ends invalid / rebuilt.

---

## `epochs` (S2, S3, S4)

Implements `PendingEpochLowerBound`, `OldestReaderEpoch`, the commit epoch the
host passes to `RuntimeHost::Commit(txn, epoch)`, and the reader epoch the
entry point passes as `ReadOptions::epoch`, wired to the engine's real
commit / abort lifecycle: `PreCommit(txn)` before the commit is visible,
`Commit(txn, epoch)` after, `Abort(txn)` on rollback.

- **Epoch order.** Per accelerator, strictly increasing and in visibility
  order: if a snapshot sees commit `n` it sees every earlier commit. Take it
  from the engine's own visibility clock (commit id, counter under the commit
  mutex, per-accelerator gate + ring); never from wall-clock time.
- **Reader epoch.** Exact: commit `c` is visible to the reader iff
  `epoch(c) <= E`. Never newer than the snapshot (torn read); never missing a
  commit visible when the read began (stale read). A reader that cannot be
  mapped (snapshot older than what the engine remembers) is reported as
  "answer from SQL", never guessed.
- **Pending window.** `PendingEpochLowerBound(txn)`: no reader below it can see
  the commit; readers from there wait for it. A snapshot taken between
  pre-commit and visibility must get an epoch below the pending commit's.
- **Floor.** `OldestReaderEpoch()` <= every open reader's epoch (may lag,
  never exceed); `kNoFloor` only when readers always read the newest.
- Two accelerators on one transaction take their places in accelerator-id
  order (the runtime does this; the slot must not add a lock order of its own).

**Harness:** concurrent writers on two toy accelerators over the real engine
commit path; record `(epoch, visible-at-snapshot)` pairs and assert epochs
increase in visibility order and `epoch(c) <= E` iff the snapshot saw `c` (the
SQL reference decides), including snapshots opened while a commit is held
between pre-commit and visibility (use a sync barrier); two open readers at
different points -> `OldestReaderEpoch()` <= the older's epoch, and it
advances after they close; `Abort` after `PreCommit` releases the place (the
next commit proceeds); no deadlock with two writers on two accelerators.

---

## `scan` (S5, S8)

Implements `OpenBuildSnapshot` / `BuildSnapshot::SnapshotEpoch`,
`WaitForPreRegistrationWriters`, `MaintenanceLookupEpoch` (only if maintenance
applies after visibility), and the engine-local typed chunked scans behind
`RunBuild` (`engine/scan.*`, native storage APIs; see `native_scan_contract.md`).

- **Snapshot.** A consistent read with epoch `b`: a commit is in it **iff** its
  epoch <= `b`. Writes continue while it is open: no lock that stops writers,
  no quiet window. It is valid until destroyed and usable from the pool thread
  that opened it.
- **Barrier.** First build only: waits until every writer that began before the
  accelerator's capture was registered has finished; false on timeout.
- **Scans.** Typed, chunked, **partitioned** (each partition a disjoint part of
  the table), run in parallel through the thread budget; 64-bit row counts and
  offsets; native engine types, no shared chunk IR, no client SQL round-trip.
  `RunBuild` calls the module's `Build` through them inside `snapshot` and
  **must call `builder.Finish()`**.

**Harness:** seed >=3 rows (one NULL); commit A; open the snapshot; commit B
**while it is open** (the commit must complete); scan: A visible, B not,
`SnapshotEpoch()` >= epoch(A) and < epoch(B); a subset projection and a filter
dropping >=1 row; exact cell values incl. NULL; force >=2 chunk callbacks and
assert their concatenation equals the golden set; `partitions = 1` and
`partitions = N` return the same multiset; barrier: a writer that began
earlier blocks `WaitForPreRegistrationWriters` until it commits, and a timeout
returns false.

---

## `runtime` (S6)

Implements the host (`api/{engine}_host.*`): the `EngineSlots` object owning a
`RuntimeHost` sized from the engine's thread setting
(`runtime::ResolveThreadBudget`), `EngineWorkOnHostThread()`, and the placement.

- **In-process.** Builds and applies run on pool threads / the writer's
  thread (`EngineWorkOnHostThread() == false`); the read path never enters an
  engine-facing thread.
- **Shipped worker.** One worker per accelerator hosting a `RuntimeHost`;
  commits cross as `wire.hpp`-encoded changes (`OfferCommit` /
  `AnnounceCommit`, `Pump` on the worker's engine thread,
  `EngineWorkOnHostThread() == true`); the writer waits bounded; a commit that
  cannot be delivered becomes a gap (the runtime rebuilds). Read threads call
  no engine API: no engine headers in their files, signals blocked. A worker
  restart is safe: the next build includes everything.
- No commit is dropped or skipped by the host; no thread pool, build
  scheduler, lock or version logic of its own.

**Harness:** toy module on the host over real writes: seed -> `BuildAll` ->
read equals golden; DML + commit -> read equals the new golden; `Invalidate()`
under a continuous writer loop -> synced within a bound while the writer keeps
committing, final answer exact; a reader transaction older than the build
reads its own snapshot's answer or gets `Fallback`, never a newer one; shipped
placement: drop one commit message -> gap -> rebuild -> exact, kill the worker
-> reads fall back exact, then hit again; `ThreadBudget::PeakThreads()` <= the
configured budget.

---

## `host` (S7)

Implements the SQL-visible entry point(s) and packaging. Loadable in-process
MODULE/extension. Server-only symbols only in the MODULE. One entry calls the
toy module: decode arguments (incl. NULL) into `QueryArguments`, call
`AcceleratorRuntime::Read(ReadOptions{epoch = S3, max_wait_us}, args, sink)`,
emit rows; on `Fallback` run `ExecuteFallback` at the reader's snapshot. Read
parallelism lives inside `Execute`: do not use the engine's own parallel
sources.

**Harness:** load the host -> call the entry -> the `runtime` golden through
SQL; NULL argument; a `Fallback` (invalid state) answered correctly through
SQL; the entry named by the registry, not by a backend list.

**Test code stays out of the production MODULE.** Gate-test UDFs and toy
bridges (test-table setup, state inspection, runtime-layer drivers, the toy
module's SQL entry) live in `backends/<engine>/tests/` and build into a
separate test-only MODULE/plugin (`<module>_test`, e.g.
`accelerator_codegen_pg_test`); the layer harnesses load that module
themselves. The production MODULE/plugin holds only real entry points
(capture hooks, query registries and their SQL entry points).

### Query registries (every engine)

The backend is generated once and must serve any number of queries without
being edited: it **names no query**.  Query pipelines add each query under
`accelerators/<bench>/q<N>/<engine>/` and plug it in with static registrars,
so the backend exposes registries plus generic dispatch over them:

- **catalog** (`backends/<engine>/api/catalog/query_catalog.{hpp,cpp}`):
  `Register…(Entry)` + a `…Registrar` struct; lookups and query lists iterate
  the registry (sorted by query id).
- **SQL entry points**: one generic implementation the host calls with the
  query's registered entry.  Where the host resolves functions by symbol name
  (UDFs, `PG_FUNCTION_INFO_V1`), the per-name symbol is a one-line shim in the
  query's directory that forwards to the generic implementation.  The SQL that
  creates the functions (extension script, `CREATE FUNCTION` list) is assembled
  from per-query fragments at build/install time, never hand-listed.
- **runtime bindings / scans**: a registry from accelerator id to the query's
  module + scan factory; the backend's scan helpers are exported in a header
  for the queries' scan code.
- Registries are plain functions in a backend `.cpp` with function-local
  static storage and stable entry addresses (not header-only template
  singletons, which can duplicate across the host library and the MODULE).
  Per-query MODULE-side code (scans using server APIs, symbol shims) is
  globbed into the MODULE from the query directories.

The build must succeed with **every** `accelerators/<bench>/q<N>/` removed
(zero registered queries), and `scripts/check_backend_query_free.py --engine
<engine>` must pass (the `engine_build` gate runs it).  Engine-level helpers a
query needs (e.g. a key-list predicate, a collation-safe literal) belong in the
backend, generic and documented; per-query code never does.
