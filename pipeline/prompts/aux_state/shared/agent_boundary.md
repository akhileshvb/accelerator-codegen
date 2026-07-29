# Agent boundary: aux-state query generation

## Agent may generate (query-specific)

- State representation (partition payload struct, partition keying)
- `Interpret*Mutation`, relevance/column watches
- Incremental accumulators and rebuild implementations
- `*MutationBatchFeatures` extraction
- `Enumerate*ActionCandidates` rules
- Initial cost formulas and calibration workload descriptors
- Aux kernel/package, diagnostics, and query-specific tests/benchmarks

## Hand-written substrate (stable — do not regenerate)

- `PartitionedStateStore` (with epoch metadata), txn buffer lifecycle, execution gate
- `AcceleratorStateRegistry` and `AcceleratorMaintenanceState` (global lazy rebuild, per-accelerator epochs)
- `ReaderMutationEpochState`, `ExecutionLoadRegistry`, `ActiveExecutionScope`
- `CoreSelectorParams`, `ActionCostEstimate`, `SelectMinCostAction`, `OnlineStats`
- `EstimateInterferenceUs`, `ActionResourceEstimate`, `PublishValidatedPartitionUpdates`
- Staged publication and commit/rollback hooks
- Shared maintenance diagnostics
- CMake scaffolding: `cmake/AuxState.cmake` (`accel_add_aux_state_query`)

## Registration checklist

See `registration_checklist.md` in this directory.

## Runtime policy

See `runtime_policy.md` in this directory. Default: hybrid cost-based selector; lazy-on-query repair in v1 with global pending rebuild state.

## Concurrency

See `concurrency_contract.md` in this directory. Agents must implement relevance filtering, bounded read paths, and NoOp maintenance for irrelevant mutations.
