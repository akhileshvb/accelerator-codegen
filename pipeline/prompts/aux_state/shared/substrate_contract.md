# Substrate contract (stable — do not regenerate)

Hand-written APIs in `include/accelerator_codegen/aux_state/`:

- **PartitionedStateStore** — partitioned payload storage with `partition_version` and `valid_through_mutation_epoch` per slot; invalidation and staged reads.
- **AcceleratorStateRegistry** — per-accelerator registration; hooks for interpreter, incremental buffer factory, mutation accumulator, maintenance hook, table dependencies.
- **AcceleratorMaintenanceState** — per-accelerator `pending_plan` / `pending_generation`, `committed_mutation_epoch`, rebuild and publish mutexes; claimed-snapshot lazy rebuild via `BeginRebuild` / `CompleteRebuild(claim, completed_plan)` with `SubtractInvalidationPlan`.
- **TxnInvalidationBuffer** — per-connection incremental buffers and txn-local bypass; lazy rebuild scheduling is **global** via maintenance state (not per-connection `SetPendingLazyRebuild` on commit).
- **ExecutionGate** — txn-safe aux vs fallback per partition: `Valid` + readable payload + no txn bypass. `reader_visible_mutation_epoch` is pinned at statement start (v1); `valid_through_mutation_epoch` records publish coverage. Unaffected partitions may remain `Valid` with older `valid_through` while unrelated mutations advance `committed_mutation_epoch`.
- **ReaderMutationEpochState** — v1 statement-start committed visibility: pins reader-visible mutation epoch per accelerator per statement.
- **ExecutionLoadRegistry** — cross-connection in-flight aux/fallback/maintenance/rebuild tracking for interference costing.
- **MaintenanceHook** — `PlanMaintenance`, `PublishMaintenance`, `DiscardMaintenance` with staged publication under `maintenance_publish_mutex`.
- **Staged publication** — `StagedPartitionUpdate.expected_partition_version`; `PublishValidatedPartitionUpdates` validates at publish time.
- **Interference model** — action-aware `EstimateInterferenceUs(action, ActionResourceEstimate, load, params)`; coefficients default to 0.

## Multi-writer / multi-connection invariants

1. Uncommitted writes remain isolated via per-connection `txn_local_bypass`.
2. Maintenance publish is serialized per accelerator; optimistic version check at publish.
3. Lazy rebuild uses claimed snapshot subtract-on-complete; never Take/Clear global pending.
4. Correctness epochs are **per-accelerator**; unrelated accelerators do not invalidate each other.
5. Reader epoch is pinned at statement start (conservative v1); do not assume naked atomic epoch reads match MVCC snapshots.

Generated code registers into this substrate; do not reimplement lifecycle or txn semantics.
