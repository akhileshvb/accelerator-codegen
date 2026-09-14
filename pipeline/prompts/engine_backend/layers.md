# Engine backend implementation layers

Each layer ships **code + a value-checked harness** and is steered by MCP
`request_gate` (`engine_build` then that layer's gate) until both pass.
Later layers may use earlier objects; they must not skip them. A later layer
may **minimally** patch a prior layer when a real bug or design flaw blocks
this layer’s contract — keep that prior harness green (add the missing assert
if it was untested). Do not rewrite prior layers for style or convenience.

The only query module allowed in this pipeline is a **toy** `AuxQueryModule`
(+ toy `InitialScan` from the scan layer onward) owned by
`tests/engine_backend/` — never generated TPC-H / SEC accelerators.

Do **not** regenerate `stateful/core|state|maintenance`.

| Order | `id` | Gate | What to implement | Harness must prove |
|------|------|------|-------------------|--------------------|
| 1 | `store` | `engine_store` | `{Engine}StateStore : AuxStateStore` in `engine::{engine}`. Wrap `PartitionedStateStore`. `TryPublish` → `TryPublishPartition` (CAS). `Read`/`CheckValidity` honor `VisibilityToken`. Keys come from published `StateImage`s — no `RegisterQueryKey`. | Golden typed payload round-trip; unknown key `Missing`; invalidate fail-closed; lost `expected_version` returns `false`; token behind publish epoch omits image / `NotVisible`. No SQL, no DML. |
| 2 | `aux_backend` | `engine_aux_backend` | `{Engine}AuxBackend : AuxEngineServices`. Ctor takes coordinator, store, visibility, cost model. Ctor calls `AttachServices(*this)`. | Toy module + **hand-built** `MutationBatch` through Capture/Abort/Commit — **not** harness `TryPublish`. Abort does not publish; Commit publishes once. Visibility / cost / `WorkerCount` come from **this** backend — not a second services class. |
| 3 | `txn` | `engine_txn` | Wire the engine’s real abort/commit onto `OnAbort` / `OnCommit` / `OnPreCommit`. | Same asserts as `aux_backend`, but the **engine txn API** fires the coordinator. Calling `OnCommit` from C++ is **not** this gate. |
| 4 | `mutations` | `engine_mutations` | Engine INSERT/UPDATE/DELETE through the real hook (trigger / WAL / listener / mutation consumer). | Value-level `MutationBatch` (table id, kind, old/new cells). DELETE asserts old-row values. `CaptureMutation(hand_built)` alone is **not** this gate. |
| 5 | `scan` | `engine_scan` | One engine-local typed callback/slot scan of a tiny **seeded** table (in-process storage / table-AM — not a client `SELECT` over the wire). Seed of `InitialScan`, **not** `AuxEngineServices`. | ≥3 rows, subset projection, one filter, exact cell values including NULL. |
| 6 | `runtime` | `engine_runtime` | Toy Runtime: `store + coordinator + backend + named scan helper + ToyModule`. `Build` calls the **scan-layer helper** and publishes via the coordinator; `Execute` reads the store with `CurrentVisibility()`. | Seed → Build → Execute matches golden rows. Then DML from `mutations` → commit → Execute rematch. |
| 7 | `visibility` | `engine_visibility` | Uncommitted writes must not appear as committed. | Snapshot taken before commit does not see the write; after commit, epoch/image changes. Sync barrier — not bare `sleep`. |
| 8 | `host` | `engine_host` | If `requires_in_process_host`: loadable in-process MODULE/extension. Server-only symbols stay in the MODULE. One SQL/UDF entry that calls the **toy** Runtime. Skip this layer when the workload does not require a host. | Load the host → call the UDF → same golden asserts as `runtime`, through SQL. |

Validation (after all layers + architecture audit) authors unseen
`tests/engine_backend/test_{engine}_adversarial.cpp` and runs `engine_adversarial`.

## Invariants

- One `AuxEngineServices` (the aux backend). Scans are a separate object.
- No portable cross-engine chunk IR.
- No reading/copying another engine’s `runtime|stateful/{other}/` tree.
- Fail closed: if the engine SDK / host cannot start, the harness exits non-zero.
