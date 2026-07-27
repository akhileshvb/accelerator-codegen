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

- `PartitionedStateStore`, txn buffer lifecycle, execution gate
- `AcceleratorStateRegistry` registration hooks
- `CoreSelectorParams`, `ActionCostEstimate`, `SelectMinCostAction`, `OnlineStats`
- Staged publication and commit/rollback hooks
- Shared maintenance diagnostics
- CMake scaffolding: `cmake/AuxState.cmake` (`accel_add_aux_state_query`)

## Registration checklist

See `registration_checklist.md` in this directory.

## Runtime policy

See `runtime_policy.md` in this directory. Default: hybrid cost-based selector; lazy-on-query repair in v1.
