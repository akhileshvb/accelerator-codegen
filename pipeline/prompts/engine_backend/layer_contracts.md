# Engine backend layer contracts (substrate)

Engine-agnostic. Adapter types live in
`accelerator_codegen::aux_state::engine::{engine}` — **never** in `core::`.
Fill `pipeline/templates/engine_backend/` (keep that namespace). Do **not**
read another engine’s `runtime|stateful/` tree.

If `backend_analysis.json` `harness_plan` contradicts this file or
`acceptance_checklist.md`, **follow this file**.

---

## `store`

**Read first:** `include/accelerator_codegen/stateful/core/aux_types.hpp`
(`AuxStateStore`, `QueryKey`, `VisibilityToken`, `ValidityCode::NotVisible`),
`include/accelerator_codegen/stateful/state/partitioned_state_store.hpp`.

`{Engine}StateStore` wraps `PartitionedStateStore`. Typed `StateImage` payloads
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
