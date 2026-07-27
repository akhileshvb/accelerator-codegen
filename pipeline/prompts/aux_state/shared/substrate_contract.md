# Substrate contract (stable — do not regenerate)

Hand-written APIs in `include/accelerator_codegen/aux_state/`:

- **PartitionedStateStore** — partitioned payload storage, invalidation, staged reads.
- **AcceleratorStateRegistry** — per-accelerator registration; hooks for interpreter, incremental buffer factory, mutation accumulator, maintenance hook, table dependencies.
- **TxnInvalidationBuffer** — per-accelerator incremental buffers; lazy rebuild plans; `SetPendingLazyRebuild(accelerator_id, plan)`.
- **ExecutionGate** — txn-safe aux vs fallback decision per partition.
- **MaintenanceHook** — `PlanMaintenance`, `PublishMaintenance`, `DiscardMaintenance` with staged publication.
- **Staged publication** — commit/rollback lifecycle for maintenance side effects.

Generated code registers into this substrate; do not reimplement lifecycle or txn semantics.
