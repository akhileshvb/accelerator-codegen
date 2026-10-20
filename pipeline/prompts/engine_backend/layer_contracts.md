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

**Engine facts.** The engine profile (`engine_profile`, in your prompt) states how
this engine does each mechanic below and lists its hazards; the requirements are
the same for every engine.

---

## Requirements every backend meets

Substrate owns commit order, epochs, versions, catch-up and builds; these are what
the engine side must hold up. Each names its layer.

- **Own writes** (`runtime`; entries in `host`). A read inside a transaction that
  wrote a table the accelerator depends on is answered from SQL at that
  transaction's own snapshot (`RuntimeHost::TxnWroteDependency`): its rows are in no
  published state. It never returns an answer without them, whether the statement
  was planned before or after the write, and whatever the isolation level.
- **Writes are judged at commit** (`capture`, `epochs`). What a write did is decided
  when its transaction commits, not when its statement ran: an aborted write is never
  visible; a statement that was already running when an accelerator registered, or
  a commit that lands after a build's snapshot without having been captured,
  invalidates the state it would be missing (`InvalidateFrom`), never goes unseen.
- **Lock discipline** (all layers). No engine call is made while holding a lock that an
  engine callback of the same engine can take. No substrate lock is held across an
  engine call. Every wait on an engine lock or on another thread inside a slot is
  bounded; expiry fails the operation (a failed build: readers fall back, the next read
  retries) and is never a hang. The engine locks that parallel scan lanes hold or
  request (profile `locking`) cannot form a cycle with a concurrent DDL on the
  scanned tables: a build ends as success or failure within its bound.
- **DDL events** (`capture`, `runtime`). TRUNCATE, DROP, RENAME, replace or recreate of
  a captured table, and a change of its columns, either reach the runtime as an
  invalidating change or are detected before the next answer; the accelerator rebuilds
  or answers from SQL. Where the engine delivers no event for one of them (profile
  `capture.ddl_events`), the backend still never answers from state built for the old
  table.
- **Abort and crash paths** (`capture`, `epochs`, `runtime`). A rollback (also of a
  subtransaction), an abort after pre-commit, a connection closed inside a
  transaction, a commit whose outcome is unknown, and the death of a worker or session
  leave no place held in commit order, no half-published epoch and no uncommitted row
  in a state; an unknown outcome invalidates.
- **Thread knobs** (`runtime`). Each phase cap is honoured through the mechanism the
  profile names for `thread_knobs`, at startup and on every later change the engine
  allows; `ACCEL_AUX_*_THREADS` override it.

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
engine's non-row change) -> the state ends invalid / rebuilt; DROP / RENAME /
recreate of a tracked table -> no read answers from the old table's state; a
statement running at registration whose transaction commits later is not lost.

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
next commit proceeds); a connection closed inside a writing transaction releases
it too; no deadlock with two writers on two accelerators.

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
`partitions = N` return the same multiset, each partition's rows arrive in table order and the same partition holds the same rows on a repeated scan, partitions overlap in time (concurrency observed) with at most the build cap of threads, a writer commit between partition starts is invisible to every partition, a NULL in a non-`Nullable` column and an oversized decimal fail loudly; barrier: a writer that began
earlier blocks `WaitForPreRegistrationWriters` until it commits, and a timeout
returns false; a DDL on the scanned table issued while a multi-partition scan is
under way ends the scan (success or failure) within its bound and never hangs.

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
- **Thread budget and phase caps.** One `ThreadBudget` from `ResolveThreadBudget(engine setting)`; the read, build and maintenance caps from `ResolveReadThreads` / `ResolveBuildThreads` / `ResolveMaintenanceThreads` with the engine's own settings when it has a settings mechanism (e.g. Postgres `accel.read_threads` / `build_threads` / `maint_threads`; `ACCEL_AUX_*_THREADS` always override), applied with `SetPhaseLimits`. A multi-process placement shares the budget across processes (`TokenSource`).
- **Table sizes and cost model.** `TableRows(table)` returns the engine's cheap row estimate (64-bit; 0 only if unknown) so `DataScaleRows` scales the maintenance policy; `CostModel(accelerator)` comes from `core::ResolveAuxCostModelPath` with the engine name, the scale tag (DuckDB: `ACCEL_AUX_BENCH_DB_LABEL`) and the per-engine override env var.
- No commit is dropped or skipped by the host; no thread pool, build
  scheduler, lock or version logic of its own.

**Harness:** toy module on the host over real writes: seed -> `BuildAll` ->
read equals golden; DML + commit -> read equals the new golden; `Invalidate()`
under a continuous writer loop -> synced within a bound while the writer keeps
committing, final answer exact; a reader transaction older than the build
reads its own snapshot's answer or gets `Fallback`, never a newer one; a read inside a
transaction that wrote a dependency table equals the SQL answer that includes those
rows; shipped
placement: drop one commit message -> gap -> rebuild -> exact, kill the worker
-> reads fall back exact, then hit again; `ThreadBudget::PeakThreads()` <= the
configured budget; each phase cap set through the engine setting and through the env override is observed by `WorkerCount()` inside the toy module's `Build` / `Execute` / maintenance (e.g. a cap of 1 gives 1); `DataScaleRows()` equals the table's row count after a bulk insert (and `ChooseMaintenance` with a model whose `calibrated_scale_rows` is a tenth of it raises the ApplyDelta cap tenfold).

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
SQL; the entry named by the registry, not by a backend list; the same read inside a
transaction that wrote a dependency table (own writes) equals SQL, also for a
statement prepared before the write where the engine has prepared statements.

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

---

## `api`

Writes `backends/{engine}/api/` and `api/README.md` against
`pipeline/prompts/engine_backend/api_contract.md` (binding; read it in full) and the
manifest format of `host_contract.md`. The slot layers built the engine side; this
layer exposes it. Move what accelerators and harnesses must reach (registries, runtime
handle, scan helpers, SQL fallback, cost-model path, thread settings, engine-exact value
helpers) into `api/`, keep everything else (`engine/`, `state/`, `tests/`) private, and
add the introspection hooks.

**Harness:** none of its own. The gate builds `api_conformance_{engine}` through
`host.sh build`: each `api/` header compiled alone with the flags of the code that
includes it, so an `api/` header that needs another include, or the engine SDK set up
first, fails here and not in the first accelerator.

Before that it runs `pipeline/gates/check_api_manifest.py` and
`scripts/check_layer_includes.py`; fix their output as it names the file and line.
