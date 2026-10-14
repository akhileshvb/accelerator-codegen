# Engine backend acceptance checklist (shared bar)

**Audience:** analysis, implementation, and architecture_audit.
**Scope:** engine-agnostic. Do **not** invent engine-specific APIs here — map
each item using this engine’s SDK hints / analysis only.

This is the **single closed set of hard rules**. Implementation must satisfy
every item before finishing. Architecture audit may **only** fail on these
`rule` ids (below). Do **not** fail for style, extra concurrency ideas, or
requirements that appear only in `backend_analysis.json` but are not listed
here — if analysis promised more, treat that as optional polish unless it
maps to a rule below.

Gates (`engine_build` plus each layer gate in `layers.md`) are
necessary but **not sufficient**: a green harness exit can still fail audit if
any checklist item is missing in source. Architecture audit may **only** fail
on these `rule` ids. Each rule is owned by one implementation layer.

---

## Rules (fixed `rule` ids)

### `native_scan` (layer `scan`)

- Adapter exposes an **engine-local** base-table scan helper (name from analysis /
  SDK hints) using **in-process / storage-AM / table-AM** APIs.
- Preferred role: seed tiny tables and feed **aux rebuild** / typed rebuild
  callbacks — **not** `ScanNativeBaseTable` → portable `DataChunk`.
- **Forbidden** as the scan implementation: client-protocol SQL round-trips
  (`SELECT … FROM …` via a network/driver client) that parse rows into a
  portable chunk IR.
- **Forbidden:** a silent no-op / empty stub for the named helper. Host builds
  may fail closed (throw / `ereport`); the in-process host/MODULE path must
  contain the real table-AM (or equivalent) implementation. A UDF that
  re-implements scan inline **without** calling the named helper is a fail —
  contract / rebuild must invoke the helper symbol.
- Contract harness **must call** that native helper (directly or via an
  in-process host/extension helper that invokes it). A harness that only runs
  client `SELECT count(*)` / status RPCs against base tables is a fail.
- If analysis documents that base consume is entirely accelerator-owned and the
  backend still ships a minimal typed rebuild scan for this rule, that is OK —
  evidence must still show a real native call site.

### `value_checked_scan` (layer `scan`)

- Seed ≥3 known rows (≥1 NULL in a nullable column when types allow).
- Scan helper uses a **proper-subset projection** (order preserved) and at
  least one filter that drops ≥1 row.
- Assert **exact** surviving row count, projected arity/types, and **cell values**
  (including NULL) in **this** engine’s native row/slot/callback representation.
  “Didn’t crash” / count-only is a fail. Do **not** require another engine’s
  row type as the assert medium.
- If the helper batches: force ≥2 consumer callbacks and assert concatenation
  equals the golden set. Observe callback counts in the **same** in-process
  session / connection as the scan (do not skip because a later RPC is a new
  backend). If it never batches, document that in a harness comment and skip
  multi-callback (do not invent fake batching).

### `mutation_hooks` (layer `mutations`)

- Drive INSERT, UPDATE, and DELETE through the engine so the adapter’s real
  trigger / WAL / listener / txn callback produces `MutationBatch`.
- Assert **value-level** batch fields relevant to the design (table id, kind,
  old/new presence and/or values) — not only “pending work” / module count.
  DELETE must assert old-row values (or explicit old-row presence + cells),
  not count/kind alone.
- Hand-built `MutationBatch` + `CaptureMutation` alone is **not** enough for
  this rule (optional unit extras allowed alongside).

### `txn_lifecycle` (layer `txn`)

- Abort: pending cleared; no publish / no committed visibility change
  (assert epoch / store read unchanged — observer bools alone are insufficient).
- Commit: publish **or** invalidate exactly once as designed; visibility /
  epoch / validity changes accordingly (assert epoch advance and/or published
  value, not only `committed=true`).
- The **engine txn API** (analysis `txn_lifecycle_plan.api`) must invoke
  `OnPreCommit` / `OnCommit` / `OnAbort`. A harness that only calls those C++
  methods is a fail for this rule.
- After abort, a fresh txn can commit successfully (no poisoned coordinator).

### `visibility` (layer `visibility`)

- Uncommitted writes must not appear as committed visibility to a reader
  snapshot taken before commit.
- After commit, committed readers see the new epoch / published image (or
  correct invalidation). Assert token / epoch inequality across the boundary.
- Prefer a second concurrent reader when the engine’s process model allows it;
  if the engine is single-connection-only, prove with snapshot/token APIs
  in-process and document why.
- Do **not** rely on bare `sleep` races to create the uncommitted window —
  use a sync primitive (locks, notify, or file barriers) so the reader observes
  the writer while it is still uncommitted.

### `store_roundtrip` (layer `store`)

- Exercise the **engine’s** `AuxStateStore` (not a pure substrate toy that never
  touches the adapter store): TryPublish → CheckValidity → Read → Invalidate,
  with value asserts and fail-closed Invalidate. Class lives in
  `aux_state::engine::{engine}`, not `core::`.
- Match chosen `store_mode` (`typed_in_process` vs `serialized_boundary`).
- `TryPublish` must call `PartitionedStateStore::TryPublishPartition` and honor
  `expected_version` (return `false` on a lost race; do not keep the new
  payload). Unconditional `PublishPartition` that always returns `true` is a
  fail. Stamp `valid_through_mutation_epoch` from a ctor epoch provider (or 0).
- `Read` / `CheckValidity` must apply `VisibilityToken.mutation_epoch` against
  that stamp: a token behind the slot is omitted / `NotVisible`, not served as
  `Valid`. Fill `StateImage.version` from the slot.
- `CheckValidity` is keyed from **published** `StateImage.accelerator` +
  partition (`QueryKey.value` = decimal partition id when non-empty). Unknown
  accelerator or unknown partition → `Missing` even if other slots are Valid.
  A `RegisterQueryKey` (or similar) API the harness must call before validity
  is a fail — publish is the index.
- For `serialized_boundary` / multi-process hosts: loaders must **refresh from
  durable state** on read/validity (not only when the in-process map is empty);
  otherwise long-lived backends keep stale epochs after peer publish.

### `aux_services` (layer `aux_backend`)

- `{Engine}AuxBackend` **is** the one `AuxEngineServices`. Ctor calls
  `coordinator.AttachServices(*this)`.
- Harness abort/commit goes through Capture + `OnAbort`/`OnCommit` (coordinator
  publishes). Direct `store.TryPublish` from the harness is not this rule.
- Abort: no published image / epoch unchanged. Commit: one image with asserted
  values. `CurrentVisibility`, cost model, and `WorkerCount()` are methods on
  **this** backend — not a second services class.

### `toy_runtime` (layer `runtime`)

- `Build` consumes base rows via the **named** scan-layer helper (same symbol
  as `native_scan`), then publishes through the coordinator/store.
- `Execute` reads the store with `backend.CurrentVisibility()`.
- Seed → Build → Execute matches golden rows; post-DML commit rematches.
  Reimplementing scan only inside `Build` / a UDF, or publishing through a
  store back door, is a fail.

### `fail_closed`

- If SDK / server / embedded host cannot start or be reached, harness exits
  non-zero (`Fail`). No SKIP-then-PASS / “PASS (structural)” only.

### `host_glue` (layer `host`; skip if the workload does not require an in-process host)

- If workload `requires_in_process_host` is true: real in-process registration
  exists (not a placeholder TU). Symbols come from this engine’s SDK hints.
- Server-only symbols from this engine’s SDK (registration, txn callbacks,
  table-AM / storage-AM, triggers, module `_init` entrypoints) belong only in
  the in-process MODULE / extension — never in a host shared library that
  standalone harnesses link.
- Do **not** recompile the same stateful TUs into both the host lib and the
  MODULE while also linking the host lib (ODR / dual-singleton). MODULE owns
  extension + bridge + server-only TUs; host lib owns substrate adapters
  without server symbols.
- If false: N/A — auditor must not invent a host requirement.

### `query_registries`

- The backend names no query: `scripts/check_backend_query_free.py --engine {engine}`
  passes (no `TpchQ<N>` / `tpch_q<N>` / `tpch/q<N>/` outside comments).
- Catalog, SQL entry points and runtime bindings are registries that queries
  fill from their own directories with static registrars; the SQL that creates
  functions is assembled from per-query fragments.
- The backend builds and loads with zero registered queries.

### `independence`

- No reading/copying another engine’s `backends/{other}/` tree.
- No dependence on generated query accelerators (`tpch` / `sec_edgar` / etc.)
  in layer harnesses.
- No reintroduction of a shared portable chunk IR.

---

## Implementation self-check (before finishing)

For each applicable rule above, record (in the agent transcript or a short
comment block at the top of the layer harness) **path + symbol** evidence.
If you cannot point to a call site that exercises the adapter path, the rule
is not done — keep fixing; do not hand off to audit hoping it will pass.

---

## Architecture audit discipline

- Use **only** the `rule` ids listed above in `violations[].rule`.
- On fail: actionable `feedback_for_implementation` that maps 1:1 to those rules.
- On pass: empty feedback; do not add “nice to have” follow-ups as failures.
- Do **not** raise a new bar that was not in this checklist between rounds.
- Prefer “adapter + harness call the real paths with value asserts” over
  demanding a redesign of already-correct internals.
  Do **not** fail solely because the scan helper is not named
  `ScanNativeBaseTable` or does not emit `DataChunk`.
