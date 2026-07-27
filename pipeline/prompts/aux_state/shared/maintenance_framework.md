# Maintenance framework (stable)

## MaintenanceAction enum

`NoOp`, `ApplyDelta`, `RebuildPartition`, `RebuildAll`, `InvalidateAndFallback`

## ActionCostEstimate

- `immediate_us` — cost paid now (write path / query blocking).
- `differential_future_query_us` — change in expected future query cost vs leaving state as-is.
- `interference_us` — contention / pipeline interference.

`TotalOverheadUs()` = sum of the three.

## Action selection

`SelectMinCostAction(SelectionInput)` compares `ActionCandidate` totals. Uses `decision_margin_us` from `CoreSelectorParams` (stable selector thresholds).

## Per-accelerator plugins (agent generates)

- `Enumerate*ActionCandidates` — which actions are legal for this mutation batch.
- Feature structs (`*MutationBatchFeatures`, `*WorkloadFeatures`).
- Eligibility rules and cost equation functions: `(features, params) → ActionCostEstimate`.
- Initial JSON coefficients loaded per accelerator (`config/q{N}_aux_cost_model.json`).

The framework only compares totals; cost equations are per-accelerator.

## Repair timing

v1: **LazyOnQuery** only. No scan work in commit pipeline.
