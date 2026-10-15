# Engine backend layer contracts (substrate)

Engine-agnostic. Adapter types live in
`accelerator_codegen::aux_state::engine::{engine}` — **never** in `core::`.
Fill `pipeline/templates/engine_backend/` (keep that namespace). Do **not**
read another engine’s `backends/<engine>/` tree.

If `backend_analysis.json` `harness_plan` contradicts this file or
`acceptance_checklist.md`, **follow this file**.

---

## `store`

**Read first:** `substrate/aux/core/aux_types.hpp` (`AuxStateStore`,
`StateImage`, `StateSnapshot`, `VisibilityToken`, `QueryKey`, `ValidityCode`,
`StagedMaintenanceOutcome`), `substrate/aux/state/versioned_aux_store.hpp`
(`VersionedAuxStore`, `Options`, `AuxReadScope`),
`substrate/aux/state/invalidation_plan.hpp`, and the "Concurrency" section of
`substrate/aux/core/README.md`.

`VersionedAuxStore` **is** the `AuxStateStore` implementation (versions,
pending commits, `expected_version` check, invalidation, locking). The engine
store **configures** it, it does not re-implement it:

```cpp
class {Engine}StateStore final : public VersionedAuxStore {
  explicit {Engine}StateStore(EpochProvider epoch = {}, FloorProvider floor = {});
};
```

| `VersionedAuxStore::Options` | Engine choice |
|-----|------|
| `epoch` | Epoch a publish / commit becomes current from. Same order as `CurrentVisibility().mutation_epoch`: a snapshot at epoch `E` must include exactly the commits whose epoch is `<= E`. None: every version is `kAllSnapshots`. |
| `floor` | Oldest snapshot epoch still open (prunes versions). None: keep only the newest version. |
| `fixed_accelerator` | Set when the engine keeps one store per accelerator (images of any other accelerator are rejected). |
| `track_transaction_dirty` | Only if one store serves one session (the flag is store-wide). |

Overriding `Read` / `ReadPartitions` / `CheckValidity` is allowed only to wrap
the `VersionedAuxStore::` call (diagnostics). No own maps, locks, slot epochs,
or key-registration API: publish is the index.

Semantics the harness asserts **through the engine class** (inherited):

| API | Behavior |
|-----|----------|
| `TryPublish(staged, expected)` | Appends a version at the epoch provider's epoch (called once). Each published partition's version becomes previous + 1 (the image's own `version` is ignored). `expected != 0` and a partition's version differs → that partition is invalidated for every snapshot and the result is `false`. |
| `Read` / `ReadPartitions` | Newest version with epoch `<= token.mutation_epoch` (`0` = newest). Omits invalid / payload-less partitions; `image.version` = partition version; `available` iff any image. |
| `CheckValidity(key, token)` | Empty `key.value` = accelerator-wide (`Valid` iff known and no invalid partition); else decimal partition id. Unknown accelerator / partition → `Missing`; invalid → `Invalidated`; no version at the token's epoch → `NotVisible`. |
| `Invalidate(plan)` | `PartitionSet` / `AllPartitions` invalid in every kept version (fail-closed). |
| `BeginCommit` / `FinishCommit` / `AbortCommit` | Driven by `AuxTransactionCoordinator` only (PRE_COMMIT / COMMIT / ABORT). Readers whose snapshot may include a pending commit wait for it. |
| `Install` / `InstallIfUnchanged` | Fresh builds (runtime layer). |

The store is thread-safe (writer-preferring rwlock); callers add no lock.
Payloads are `shared_ptr<const void>` (`StateImage::Make` / `As<T>`); a reader
holds the `shared_ptr` while using it. The runtime opens one `AuxReadScope`
per query.

**Harness (no SQL, no DML):** typed payload round-trip (`As<T>`, version 1
after first publish); unknown accelerator, unknown partition, unpublished
`"0"` → `Missing`; empty key accelerator-wide; `Invalidate` (`PartitionSet`
and `AllPartitions`) → `Invalidated` and omitted from `Read`; **expected
version:** mismatched `expected_version` returns `false` and the partition
reads `Invalidated`, a republish with `0` succeeds; **epoch:** a provider
returning `N` is called once per publish, a token at `N-1` omits the image /
`NotVisible`, at `N` is `Valid`; no provider → visible to every token;
`ReadPartitions` subset / unknown id.

---

## `aux_backend`

**Read first:** `substrate/aux/core/aux_registry.hpp`
(`AuxTransactionCoordinator`: `AttachServices`, `OnMutation[AtDepth]`,
`PreCommit`, `Commit`, `Abort`, subtransaction calls),
`substrate/aux/core/aux_types.hpp` (`AuxEngineServices`, `AuxQueryModule`,
`MutationBatch`).

- Ctor arguments: coordinator, store, visibility callback, cost model.
  **Ctor body must call `coordinator.AttachServices(*this)`.** The aux_backend
  template is wrong if it omits this — add it.
- This class **is** the one `AuxEngineServices`. Do not introduce a second
  services object for visibility / cost / `WorkerCount`. `StateStore()`
  returns the engine store; `CurrentVisibility().mutation_epoch` is in the
  store epoch provider's order.
- Override `AllowsSynchronousRebuildOnCommit` / `ReadersCanBuildState` /
  `WorkerCount` when the engine differs from the defaults (from analysis).
- Harness drives a toy `AuxQueryModule` + **hand-built** `MutationBatch` through
  `CaptureMutation` / `OnAbort` / `OnCommit` — **not** `store.TryPublish` from
  the test. Forward those to `coordinator.OnMutation` / `Abort()` / `PreCommit()` /
  `Commit()` (those are the coordinator names). Abort: no image / epoch unchanged.
  Commit: exactly one published image with asserted values.
- Assert `CurrentVisibility`, `MaintenanceCostModel`, and `WorkerCount()` on
  **that** backend instance.

---

## `txn`

Same abort/commit value asserts as `aux_backend`, but the **engine txn API**
from analysis (`RegisterXactCallback`, session hooks, …) must fire
`OnPreCommit` / `OnCommit` / `OnAbort`. A harness that only calls those C++
methods is **not** this layer. `OnPreCommit` runs before the commit is
visible to other snapshots (it marks the store pending), `OnCommit` after it
(it publishes the version at the commit's epoch); the bridge never calls the
store's `BeginCommit` / `FinishCommit` itself. Server-only symbols stay in the MODULE (see
`host_glue`). After abort, a fresh txn can commit.

---

## `mutations`

INSERT / UPDATE / DELETE through the real engine hook. Assert `MutationBatch`
table id, kind, old/new cells; DELETE asserts old-row values.
`CaptureMutation(hand_built)` alone is not this gate.

---

## `scan`

Named engine-local table-AM / storage-AM helper (symbol from analysis). Seed
≥3 rows (include NULL), subset projection, one filter, exact cells. Runtime
must **call this helper** — do not reimplement scan only inside a UDF.

---

## `runtime`

Toy Runtime = store + coordinator + **this** aux backend + named scan helper +
toy module. `Build` scans via the **named** helper then publishes through the
coordinator (not a store back door). `Execute` reads the store with
`CurrentVisibility()`. Seed → Build → Execute matches golden rows; then
mutations-layer DML + commit rematches.

---

## `visibility`

Uncommitted work must not be `Read` as committed. Relies on the store's
version selection by `VisibilityToken.mutation_epoch` **plus** the
coordinator publishing only at commit. Snapshot before commit ≠ after.
Sync barrier (locks / notify), not `sleep`.

---

## `host`

Loadable MODULE/extension. Server-only symbols only in the MODULE. One SQL/UDF
calls the toy Runtime; same golden as `runtime`.

**Test code stays out of the production MODULE.** Gate-test UDFs and toy
bridges (test-table setup, state inspection, runtime-layer drivers, the toy
Runtime's SQL entry) live in `backends/<engine>/tests/` and build into a
separate test-only MODULE/plugin (`<module>_test`, e.g.
`accelerator_codegen_pg_test`); the layer harnesses load that test module
themselves. The production MODULE/plugin used by accelerators and experiments
holds only real entry points (txn/trigger hooks, mutation capture, query
registries and their SQL entry points) and no function that creates tables
or exposes internals.

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
