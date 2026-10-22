# Read-path scaffold (agent-generated `aux/module.cpp`)

Port from shipped peer modules under `accelerators/{bench}/q*/<engine>/aux/module.cpp` (the workload's reference accelerators). Every aux query implements the same `AuxQueryModule` surface.

## Module methods (required)

| Method | Responsibility |
|--------|----------------|
| `CheckValidity` | `services.StateStore().CheckValidity({Id(), window_partitions}, services.CurrentVisibility())` |
| `Execute` | Fast path: read only partitions overlapping the query window; emit via `AuxRowSink` |
| `ExecuteFallback` | Recompute from base tables when state is missing, stale, or txn-blocked |
| `StageMaintenance` | Build `MaintenanceRequest`, call `services.ChooseMaintenance(...)`, materialize updates |

## Single-partition pattern (Q14)

```cpp
bool TpchQ14Module::CheckValidity(core::AuxEngineServices &services,
                                  const core::QueryArguments &) const {
  return services.StateStore()
      .CheckValidity({Id(), {}}, services.CurrentVisibility())
      .IsValid();
}

void TpchQ14Module::Execute(core::AuxEngineServices &services,
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

## Window / prefix gates (multi-partition queries)

When the query spans multiple partitions (Q1 prefix, Q10/Q12 date windows):

1. Compute the partition ID set for the query window only.
2. Pass that set to `CheckValidity` / `ReadPartitions` — never scan all slots.
3. If any required partition is invalid or absent → `ExecuteFallback` for the window.
4. Do not reject valid partitions solely because their `valid_through` epoch lags unrelated writes.

## Parallel `Execute`

When the read is proportional to selected state (many partitions or groups), take the snapshot first (`ReadHitPartitions`), then split with `core::ParallelReduce` over the borrowed images (`substrate_primitives.md`; slice rules in `engine_interfaces.md`). A read bounded to one small partition stays one slice.

## Maintenance hook (write path)

In `InterpretMutation` / `AccumulateMutation` / `StageMaintenance`:

- Irrelevant mutations → empty `InvalidationPlan` or `MaintenanceAction::NoOp`
- Relevant maintenance → `services.ChooseMaintenance` with accurate row/partition counts
- Populate `interference_us` via calibrated `CostModelParams` fields, not ad-hoc globals

## No state encoding

State images stay typed in the process that hosts the runtime; do not write
`Encode*` / `Decode*` for state. Only captured batches, arguments and result
rows cross a process boundary, and the substrate (`runtime/wire.hpp`) encodes them.

## Reference files

| Pattern | Reference |
|---------|-----------|
| Single-partition | a peer with one window partition |
| Multi-partition window | a peer with windowed partitions |
| Prefix / cumulative | a peer with prefix state |
| Append workload | a peer with append-only state |
