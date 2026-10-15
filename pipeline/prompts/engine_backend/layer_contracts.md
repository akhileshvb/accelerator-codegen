# Engine backend layer contracts (substrate)

Engine-agnostic. Adapter types live in
`accelerator_codegen::aux_state::engine::{engine}` — **never** in `core::`.
Fill `pipeline/templates/engine_backend/` (keep that namespace). Do **not**
read another engine’s `backends/<engine>/` tree.

If `backend_analysis.json` `harness_plan` contradicts this file or
`acceptance_checklist.md`, **follow this file**.

---

## `store`

**Read first:** `substrate/aux/core/aux_types.hpp`
(`AuxStateStore`, `QueryKey`, `VisibilityToken`, `ValidityCode::NotVisible`),
`substrate/aux/state/versioned_aux_store.hpp`.

`{Engine}StateStore` wraps `VersionedAuxStore`. Typed `StateImage` payloads
for `typed_in_process`.

| API | Must |
|-----|------|
| `TryPublish` | Call `TryPublishPartition` (not `PublishPartition`). Forward `expected_version`. Return `false` if any slot loses the race. Stamp `valid_through_mutation_epoch` from a ctor epoch provider, or `0`. |
| `Read` | Copy `visibility` onto the snapshot. Omit slots that are not `Valid`, have no payload, or have `valid_through_mutation_epoch > visibility.mutation_epoch`. Fill `image.version` from `GetPartitionVersion`. |
| `CheckValidity` | Keyed by **published** `StateImage.accelerator` + partition. Treat non-empty `QueryKey.value` as the decimal partition id. Unknown accelerator or unknown partition → `Missing` even if other slots are Valid. Token behind the slot epoch → `NotVisible`. Invalid slot → `Invalidated`. |
| `Invalidate` | `InvalidatePartition` / `InvalidateAll` per plan scope. |

**Forbidden:** `RegisterQueryKey` or any side-channel index the harness must
call before `CheckValidity`. Publish itself is the index.

**Harness (no SQL):** golden typed payload round-trip (`StateImage::As`);
unknown key `Missing`; invalidate fail-closed; **CAS:** a second `TryPublish`
with a non-matching `expected_version` returns `false` and does not keep the
new payload; **token:** publish with epoch provider `> 0`, `Read`/`CheckValidity`
with a smaller `mutation_epoch` omits the image / `NotVisible`.

---

## `aux_backend`

**Read first:** `aux_registry.hpp` (`AttachServices`, `OnMutation` /
`OnCommit` / `OnAbort`), `aux_types.hpp` (`AuxEngineServices`, `WorkerCount`).

- Ctor arguments: coordinator, store, visibility callback, cost model.
  **Ctor body must call `coordinator.AttachServices(*this)`.** The aux_backend
  template is wrong if it omits this — add it.
- This class **is** the one `AuxEngineServices`. Do not introduce a second
  services object for visibility / cost / `WorkerCount`.
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
methods is **not** this layer. Server-only symbols stay in the MODULE (see
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

Uncommitted work must not be `Read` as committed. Relies on store token/epoch
filtering **plus** not publishing until commit. Snapshot before commit ≠ after.
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
