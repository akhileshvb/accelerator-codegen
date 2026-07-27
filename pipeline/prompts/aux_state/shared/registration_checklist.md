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

Txn buffer: `SetPendingLazyRebuild(accelerator_id, plan)` for lazy repair.
