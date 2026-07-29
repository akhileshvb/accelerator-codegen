# Registration checklist

1. Implement `RegisterQ{{query_number}}AuxAccelerator(connection)` in `*_aux_dependencies.cpp`.
2. Register: mutation interpreter, incremental buffer factory, mutation accumulator, maintenance hook, table dependencies.
3. Expose `{{aux_function_name}}` table function and build/rebuild helpers.
4. Add sources via `accel_add_aux_state_query(QUERY_NUM {{query_number}} ...)` in `CMakeLists.txt`.
5. Wire `RegisterQ{{query_number}}AuxAccelerator` in `src/register.cpp` after `InstallAcceleratorSubstrate`.

Registry hooks:

- `RegisterIncrementalBufferFactory(accelerator_id, ...)`
- `RegisterMutationAccumulator(accelerator_id, ...)`
- `RegisterMaintenanceHook(accelerator_id, ...)`

Lazy repair: on relevant maintenance commit, merge into **global** `AcceleratorMaintenanceState` pending plan (per-accelerator). Any connection may apply via `EnsurePending*RebuildApplied` — not per-connection `SetPendingLazyRebuild` on commit.

Concurrency: read `concurrency_contract.md` — irrelevant mutations NoOp, bounded read path, per-accelerator epochs.
