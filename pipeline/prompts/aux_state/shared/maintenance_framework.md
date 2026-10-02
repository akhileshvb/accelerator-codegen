# Maintenance framework (stable)

## MaintenanceAction enum

`NoOp`, `ApplyDelta`, `RebuildPartition`, `RebuildAll`, `InvalidateAndRebuildOnRead`

Every accelerator must handle every action (the same set everywhere):

- `ApplyDelta`: update the state from the buffered rows. If a change cannot be
  applied exactly (e.g. an UPDATE that moves existing contributions the deltas
  do not carry), return `RebuildAll` instead; never drop or approximate it.
- `RebuildPartition`: rebuild the touched partitions. An accelerator without
  finer partitions rebuilds its whole state (declare
  `partition_rebuild_supported = false` in the request so it is priced as a
  full rebuild).
- `RebuildAll`: rebuild everything.
- `InvalidateAndRebuildOnRead`: invalidate; the next reader rebuilds and
  answers from the state. Not offered by engines whose readers cannot build
  the state at their snapshot (the substrate turns it into `RebuildAll`).

`ExecuteFallback` (answer from the tables) is a last resort for a reader no
state can serve, never a maintenance strategy.

## ActionCostEstimate

- `immediate_us` — cost paid now (write path / query blocking).
- `differential_future_query_us` — change in expected future query cost vs leaving state as-is.
- `interference_us` — additional system-wide latency from concurrent readers/writers.

`TotalOverheadUs()` = sum of the three.

## Interference costing

Set `interference_us` on each `ActionCandidate` using calibrated fields from `CostModelParams`:

- `interference_rebuild_per_query_us`
- `interference_rebuild_scan_byte_us`
- `interference_delta_per_query_us`
- `interference_invalidate_per_query_us`
- `interference_query_slowdown_factor`

Coefficients live in `config/q{N}_aux_cost_model.json` and are **calibrated** by the `aux_cost_model_calibrate` gate (multi-connection readers + writers + rebuild-under-load). Populate `interference_us` in `StageMaintenance` when building candidates passed to `services.ChooseMaintenance`.

## Action selection

`SelectMinCostAction(SelectionInput)` compares `ActionCandidate` totals. Invoked via `AuxEngineServices::ChooseMaintenance` using `decision_margin_us` from `CostModelParams`.

## Per-accelerator plugins (agent generates in `aux/module.cpp`)

- Legal actions for each mutation batch (delta vs rebuild vs invalidate)
- Feature counts passed into `MaintenanceRequest` (rows touched, partitions touched, fanout)
- Cost estimates using `CostModelParams` loaded from per-query JSON
- Initial JSON at `config/q{{query_number}}_aux_cost_model.json`

The framework only compares totals; cost equations are per-accelerator.

## Rebuild vs invalidate

When the selector chooses `RebuildPartition` or `RebuildAll`, maintenance **must materialize fresh partition images** (resync/rebuild) so later queries can hit aux again. **Invalidate-only / `InvalidateAndRebuildOnRead` is not a substitute** for a selected rebuild — that leaves cold misses and defeats the cost model.

## Repair timing

v1: maintenance runs at commit via `StageMaintenance` outcomes; readers wait for a pending commit and read its published state. Only `InvalidateAndRebuildOnRead` defers work, to the next reader, which rebuilds rather than falls back.
