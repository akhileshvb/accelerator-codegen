# Read-path scaffold (agent-generated `*_aux_package.cpp`)

Port from `src/aux_state/q14/q14_aux_package.cpp` (single-partition) or `q1`/`q10`/`q12` (window/prefix gates). Every aux table function must follow this shape.

## Global state

```cpp
struct Q{{query_number}}AuxGlobalState : public duckdb::GlobalTableFunctionState {
	duckdb::idx_t MaxThreads() const override { return 1; }
};
```

Single-threaded per connection — required for multi-connection correctness.

## Local state init (statement start)

At the start of `InitQ{{query_number}}AuxLocalState` (or equivalent):

1. `EnsurePendingQ{{query_number}}RebuildApplied(context)` — apply global lazy rebuild for this accelerator.
2. `GetOrCreateTxnInvalidationBuffer(context)` — per-connection txn bypass.
3. `AcceleratorStateRegistry::GetOrCreate(*context.db)` → `GetStore(accelerator_id)` + `GetMaintenanceState(accelerator_id)`.
4. **Pin reader epoch** (once per statement):
   ```cpp
   const auto reader_epoch = aux_state::GetReaderVisibleMutationEpoch(
       context, kQ{{query_number}}AuxAcceleratorId, maintenance.GetCommittedMutationEpoch());
   ```
5. **Execution gate** over only the query window (single partition via `CheckExecutionGate`, or a query-specific `CheckQ{{query_number}}*ExecutionGate`):
   ```cpp
   const auto gate = aux_state::CheckExecutionGate(store, *txn_buffer, partition, reader_epoch);
   // gate.use_aux → fast path when partition Valid; gate.committed_invalid / gate.bypassed_by_txn → fallback
   ```
6. **`ActiveExecutionScope`** in local state for the full table-function lifetime (`AuxQuery` or `FallbackQuery`).

## Window / prefix gates (multi-partition queries)

When the query spans multiple partitions, implement `CheckQ{{query_number}}*ExecutionGate` in `*_aux_state.cpp`:

- Iterate **only partitions overlapping the query window** (skip `Absent` unless the design requires otherwise).
- For each present partition: txn bypass → `bypassed_by_txn`; `Invalid` → `committed_invalid`. Relevant mutations must invalidate or republish — do not reject `Valid` partitions solely on `valid_through < reader_epoch`.
- Do **not** scan all slots in `PartitionedStateStore` — bound work to the window.

## Bounded read iteration

Fast-path reads must merge/scan only in-window partitions (e.g. `VisitReadablePartitionsInRange`, prefix up to cutoff, or a single partition key). Never iterate `TotalPartitionSlots()` on the hot path.

## Maintenance hook (write path)

In `*_mutation_features.cpp` / `*_maintenance_policy.cpp`:

- Irrelevant mutations → `MaintenanceAction::NoOp`; do not bump `committed_mutation_epoch` without a publish effect.
- Relevant maintenance → merge into global pending plan; publish with `valid_through_mutation_epoch` at commit.

## Reference files

| Pattern | Reference |
|---------|-----------|
| Single-partition gate | `q14_aux_package.cpp`, `q3_aux_package.cpp` |
| Prefix gate | `q1_aux_state.cpp` (`CheckQ1PrefixExecutionGate`) |
| Date-window gate | `q10_aux_state.cpp`, `q12_aux_state.cpp` |
