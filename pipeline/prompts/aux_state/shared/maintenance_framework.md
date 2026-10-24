# Maintenance framework

## Actions

`MaintenanceAction`: `NoOp`, `ApplyDelta`, `RebuildPartition`, `RebuildAll`, `InvalidateAndRebuildOnRead`. Every accelerator handles all five.

- `ApplyDelta`: update from the buffered rows. If a change cannot be applied exactly (e.g. an UPDATE moving contributions the deltas do not carry), return `RebuildAll`; never drop or approximate it.
- `RebuildPartition`: rebuild the touched partitions. Without finer partitions, rebuild the whole state and declare `partition_rebuild_supported = false` so it is priced as a full rebuild.
- `RebuildAll`: rebuild everything.
- `InvalidateAndRebuildOnRead`: invalidate; the next reader rebuilds and answers from state. Engines whose readers cannot build state at their snapshot get `RebuildAll` instead (substrate).

A selected rebuild must materialize fresh partition images (resync) so later queries hit aux; invalidate-only is not a substitute. `ExecuteFallback` (answer from tables) serves a reader no state can serve; it is never a maintenance strategy.

Maintenance runs at commit through `StageMaintenance`; readers wait for a pending commit and read its published state. Only `InvalidateAndRebuildOnRead` defers work, to the next reader, which rebuilds instead of falling back.

## Cost and selection

`ActionCostEstimate`: `immediate_us` (paid now), `differential_future_query_us` (change in expected future query cost), `interference_us` (system-wide latency from concurrent readers/writers); `TotalOverheadUs()` is the sum.

Set `interference_us` on each `ActionCandidate` from the calibrated `CostModelParams` fields `interference_rebuild_per_query_us`, `interference_rebuild_scan_byte_us`, `interference_delta_per_query_us`, `interference_invalidate_per_query_us`, `interference_query_slowdown_factor`. Always pick the action through `services.ChooseMaintenance` (`SelectMinCostAction` over candidate totals, `decision_margin_us` from the model), so decisions and costs are logged; never hard-wire an action. The framework only compares totals; the cost equations are the accelerator's.

Per accelerator (in `aux/module.cpp`): legal actions per mutation batch; feature counts in `MaintenanceRequest` (rows touched, partitions touched, fan-out); estimates from `CostModelParams`.

Cost model JSON: `accelerators/<bench>/q{N}/config/q{N}_aux_cost_model.json` (initial values from the agent, `interference_*` calibrated by the `aux_cost_model_calibrate` gate under readers, writers and rebuild-under-load). `q{N}/cost_model.hpp` names that directory (`AuxCostModelLocation`); every engine's registration passes it, the substrate picks the `_<engine>` / `_sf<N>` variant.

## Refresh-shaped writes

A bulk refresh inserts or deletes a small fraction of a large table (and matching related rows) with keys spread uniformly, so it touches nearly every partition, shard, chunk and page. Maintenance is synchronous at commit, and every accelerator in the process maintains in parallel under the same cap. Design and calibrate for this shape, not only narrow-range point writes. Copy-on-write units and containers: `cow_primitives.md`.

- **Cost = delta size plus a small per-commit constant.** Calibrate and validate on bulk inserts/deletes spread over keys; fitting tiny UPDATEs makes the policy choose `RebuildAll` over `ApplyDelta` for refresh-sized deltas.
- **Index new rows in a small delta** and merge lazily/amortized; never rebuild a touched unit's index per append.
- **No size guard that forces a rebuild** outside the cost model. Bound overlays by incremental compaction proportional to recent changes; every rebuild is a priced `ChooseMaintenance` candidate.
- **Keep what maintenance needs in state** (per-key sums and counts, multisets) instead of re-reading base tables. If a rescan is unavoidable, issue one set-oriented query (one semi-join), not chunked IN-lists per key.
- **Parallelize independent per-partition and per-key work** with `core::ParallelEach` / `ParallelReduce` under the maintenance cap (fork-join rules in `engine_interfaces.md`: engine lookups first and serially, slices own disjoint keys). A serial per-row loop over a refresh dominates at large scale factors.
