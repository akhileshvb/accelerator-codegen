# Substrate contract (stable — do not regenerate)

Hand-written APIs in `substrate/aux/`:

- **VersionedAuxStore** (`state/versioned_aux_store.hpp`) — partitioned payload storage with per-slot versions; invalidation and staged reads via `AuxStateStore`.
- **Runtime skeleton** (`runtime/`) — `RuntimeHost` / `AcceleratorRuntime`: per-transaction capture, commit ordering per accelerator, maintenance of the state at commit, background builds with catch-up, reads at a snapshot's epoch, the thread budget. Backends implement its slots (`runtime/engine_slots.hpp`); modules never see it except through `AuxEngineServices`.
- **AuxTxnBuffer** (`core/aux_txn_buffer.hpp`) — the incremental buffer one commit's changes accumulate into.
- **AuxEngineServices** (`core/aux_engine_services.hpp`) — visibility token, state store access, cost model, `ChooseMaintenance`, `ExpectedQueriesBeforeNextWrite`.
- **Maintenance selector** (`maintenance/action_selector.hpp`) — `SelectMinCostAction` compares `ActionCandidate` totals using `CostModelParams` (including `interference_*` coefficients).
- **Calibration** (`core/aux_calibration.hpp`) — engine-neutral timing runner; adapters supply build/delta/rebuild callbacks.

## Visibility and correctness

1. A transaction's captured changes stay with it until it commits; uncommitted writes are never visible to state readers.
2. Maintenance of one accelerator runs in commit order and publishes a version at the commit's epoch; a read resolves the version at its own snapshot's epoch.
3. Correctness epochs are **per-accelerator**; unrelated modules do not invalidate each other.
4. `CheckValidity` on `StateStore` decides aux vs fallback for the query window — agents implement `AuxQueryModule::CheckValidity` and `Execute`/`ExecuteFallback` accordingly.
5. Irrelevant mutations must produce empty plans or `MaintenanceAction::NoOp` without side effects.

## Engine backends

Owned by `workload_kind: engine_backend` under `backends/{engine}/`: they implement the runtime's slots (capture, epochs, build snapshot, placement, entry points, scans); see `shared/engine_interfaces.md`. State images stay typed in the process that hosts the runtime; only captured batches, arguments and result rows cross a process boundary.

Generated query code implements `AuxQueryModule` and registers from its own directory (catalog entry + engine registrars; see `shared/generated_layout.md`); do not reimplement lifecycle, txn semantics, or backends.
