# Maintenance framework (stable)

## MaintenanceAction enum

`NoOp`, `ApplyDelta`, `RebuildPartition`, `RebuildAll`, `InvalidateAndFallback`

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

## Repair timing

v1: **LazyOnQuery** via `StageMaintenance` outcomes. Relevant maintenance commits merge invalidation plans; readers call `CheckValidity` and fall back when state is not ready.
