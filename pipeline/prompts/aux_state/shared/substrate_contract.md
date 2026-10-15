# Substrate contract (stable — do not regenerate)

Hand-written APIs in `substrate/aux/`:

- **VersionedAuxStore** (`state/versioned_aux_store.hpp`) — partitioned payload storage with per-slot versions; invalidation and staged reads via `AuxStateStore`.
- **AuxRegistry** (`core/aux_registry.hpp`) — transaction coordinator registering `AuxQueryModule` instances; routes mutations, txn buffers, and staged maintenance across modules.
- **AuxTxnBuffer** (`core/aux_txn_buffer.hpp`) — per-connection incremental buffers and txn-local dirty tracking.
- **AuxEngineServices** (`core/aux_engine_services.hpp`) — visibility token, state store access, cost model, `ChooseMaintenance`, `ExpectedQueriesBeforeNextWrite`.
- **Maintenance selector** (`maintenance/action_selector.hpp`) — `SelectMinCostAction` compares `ActionCandidate` totals using `CostModelParams` (including `interference_*` coefficients).
- **Calibration** (`core/aux_calibration.hpp`) — engine-neutral timing runner; adapters supply build/delta/rebuild callbacks.

## Visibility and correctness

1. Uncommitted writes remain isolated via per-connection txn buffers.
2. Maintenance publish is serialized; optimistic version checks at `TryPublish`.
3. Correctness epochs are **per-accelerator**; unrelated modules do not invalidate each other.
4. `CheckValidity` on `StateStore` decides aux vs fallback for the query window — agents implement `AuxQueryModule::CheckValidity` and `Execute`/`ExecuteFallback` accordingly.
5. Irrelevant mutations must produce empty plans or `MaintenanceAction::NoOp` without side effects.

## Engine adapters

Owned by `workload_kind: engine_backend` under `backends/{engine}/`.

- **typed_in_process** — typed `StateImage` payloads; no byte serialization on the hot path.
- **serialized_boundary** — encode/decode only at a process / shared-memory boundary (`byte_stream.hpp`); modules still see typed snapshots after decode.

Generated query code implements `AuxQueryModule` and registers from its own directory (catalog entry + engine registrars; see `shared/generated_layout.md`); do not reimplement lifecycle, txn semantics, or engine adapters.
