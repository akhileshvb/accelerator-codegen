# Read-path scaffold (`aux/module.cpp`)

Port from shipped peers `accelerators/<bench>/q*/<engine>/aux/module.cpp`: a peer with one window partition, windowed partitions, prefix/cumulative state, or append-only state, whichever matches.

| Method | Responsibility |
|---|---|
| `CheckValidity` | `services.StateStore().CheckValidity({Id(), window_partitions}, services.CurrentVisibility())` |
| `Execute` | Fast path: read only partitions overlapping the query window; emit through `AuxRowSink` |
| `ExecuteFallback` | Default `services.AnswerFromSql` (SQL at the reader's snapshot) when state is missing, stale or txn-blocked |
| `StageMaintenance` | Build `MaintenanceRequest`, call `services.ChooseMaintenance(...)`, materialize updates |

```cpp
// CheckValidity: services.StateStore().CheckValidity({Id(), {}}, services.CurrentVisibility()).IsValid()
void ExampleModule::Execute(core::AuxEngineServices &services,
                            const core::QueryArguments &arguments,
                            core::AuxRowSink &sink) {
  if (!services.StateStore()
           .CheckValidity({Id(), {}}, services.CurrentVisibility())
           .IsValid()) {
    ExecuteFallback(services, arguments, sink);
    return;
  }
  // Load only the target partition(s), emit result
}
```

## Multi-partition windows

1. Compute the partition id set for the query window only.
2. Pass it to `CheckValidity` / `ReadPartitions`; never scan all slots.
3. If a required partition is invalid or absent, `ExecuteFallback` for the window.
4. Do not reject valid partitions only because their `valid_through` epoch lags unrelated writes.

## Parallel `Execute`

When the read is proportional to selected state, take the snapshot first (`ReadHitPartitions`), then split with `core::ParallelReduce` over the borrowed images (`substrate_primitives.md`; slice rules in `engine_interfaces.md`). A read bounded to one small partition stays one slice.

Write path: `concurrency_contract.md` (irrelevant mutations) and `maintenance_framework.md` (`ChooseMaintenance` with accurate row/partition counts, `interference_us` from calibrated `CostModelParams`, not ad-hoc globals).

State images stay typed in the hosting process: write no `Encode*` / `Decode*` for state (`runtime/wire.hpp` encodes batches, arguments and rows).
