# Maintenance framework (stable)

## MaintenanceAction enum

`NoOp`, `ApplyDelta`, `RebuildPartition`, `RebuildAll`, `InvalidateAndFallback`

## ActionCostEstimate

- `immediate_us` — cost paid now (write path / query blocking).
- `differential_future_query_us` — change in expected future query cost vs leaving state as-is.
- `interference_us` — additional system-wide latency expected from choosing this action under current execution load.

`TotalOverheadUs()` = sum of the three.

## Action-aware interference

```cpp
EstimateInterferenceUs(
    MaintenanceAction action,
    const ActionResourceEstimate &resources,
    const ExecutionLoadSnapshot &load,
    const CostModelParams &params);
```

`ActionResourceEstimate` may include scan bytes, duration, partitions locked, and expected overlapping queries. Coefficients in `config/q{N}_aux_cost_model.json` (`interference_*`) are **calibrated** by `aux_cost_model_calibrate` (multi-connection readers + writers + rebuild-under-load). Maintenance selection must call `EstimateInterferenceUs` in cost formulas.

`ExecutionLoadRegistry` tracks in-flight aux queries, fallback queries, maintenance, and rebuilds. Instrument full table-function lifetime via `ActiveExecutionScope` in query local state.

## Action selection

`SelectMinCostAction(SelectionInput)` compares `ActionCandidate` totals. Uses `decision_margin_us` from `CoreSelectorParams` (stable selector thresholds).

## Per-accelerator plugins (agent generates)

- `Enumerate*ActionCandidates` — which actions are legal for this mutation batch.
- Feature structs (`*MutationBatchFeatures`, `*WorkloadFeatures`); populate `active_query_count` from execution load snapshot.
- Eligibility rules and cost equation functions: `(features, params) → ActionCostEstimate` including `interference_us`.
- Initial JSON coefficients loaded per accelerator (`config/q{N}_aux_cost_model.json`).

The framework only compares totals; cost equations are per-accelerator.

## Repair timing

v1: **LazyOnQuery** only. Global pending rebuild merged on maintenance commit; any connection may apply via `EnsurePending*RebuildApplied`.
