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

Coefficients live in `accelerators/tpch/q{N}/config/q{N}_aux_cost_model.json` and are **calibrated** by the `aux_cost_model_calibrate` gate (multi-connection readers + writers + rebuild-under-load). Populate `interference_us` in `StageMaintenance` when building candidates passed to `services.ChooseMaintenance`.

## Action selection

`SelectMinCostAction(SelectionInput)` compares `ActionCandidate` totals. Invoked via `AuxEngineServices::ChooseMaintenance` using `decision_margin_us` from `CostModelParams`.

## Per-accelerator plugins (agent generates in `aux/module.cpp`)

- Legal actions for each mutation batch (delta vs rebuild vs invalidate)
- Feature counts passed into `MaintenanceRequest` (rows touched, partitions touched, fanout)
- Cost estimates using `CostModelParams` loaded from per-query JSON
- Initial JSON at `accelerators/tpch/q{{query_number}}/config/q{{query_number}}_aux_cost_model.json`
- `accelerators/tpch/q{{query_number}}/cost_model.hpp` names that directory (`AuxCostModelLocation`); every engine's registration passes it (no hard-coded JSON path); the substrate picks the `_<engine>` / `_sf<N>` variant

The framework only compares totals; cost equations are per-accelerator.

## Rebuild vs invalidate

When the selector chooses `RebuildPartition` or `RebuildAll`, maintenance **must materialize fresh partition images** (resync/rebuild) so later queries can hit aux again. **Invalidate-only / `InvalidateAndRebuildOnRead` is not a substitute** for a selected rebuild — that leaves cold misses and defeats the cost model.

## Repair timing

v1: maintenance runs at commit via `StageMaintenance` outcomes; readers wait for a pending commit and read its published state. Only `InvalidateAndRebuildOnRead` defers work, to the next reader, which rebuilds rather than falls back.

## Refresh-shaped writes (TPC-H RF1/RF2, all accelerators in one process)

**Copy-on-write comes from `accelerators/prims/`** (`cow_primitives.md`): the substrate versions each partition payload as an opaque immutable pointer, and a commit produces a new version that shares untouched structure.

An RF inserts or deletes ~0.1% of orders (+ their lineitems) with keys spread
uniformly over the whole key space, so it touches nearly every partition,
shard, chunk and page. Maintenance is synchronous at commit; design and
calibrate for this shape, not only for narrow-range point writes.

- **COW unit = O(changed rows).** A unit that is cloned whole on first touch
  costs the whole state per RF (measured: ~4M part records copied per RF, ~3 s;
  ~13k order shards of ~1.8k orders, ~2.3 s; whole 1024-entry chunks). Make
  the copy cost per touched unit proportional to the rows changed in it:
  `shared_ptr`'d records/entries (a slot copy is pointer copies), small
  chunks, persistent maps with small leaves, or a hot/cold split (immutable
  shared base + small mutable per-version overlay). Batch all changes per
  unit so each unit is copied once per commit.
- **Never rebuild a touched unit's index on append.** Index the new tail in a
  small delta and merge lazily/amortized (a shard's token index re-merged per
  new comment cost seconds per RF).
- **No hard-coded size guard that forces a full rebuild** outside the cost
  model (e.g. `kMaxOverlayWeight = 400000` caused a 35 s `RebuildAll` every ~3
  RFs, invisible to the policy). Bound overlays by incremental compaction
  proportional to recent changes; any rebuild must be a priced candidate.
- **Do not re-read base tables for touched keys if the delta suffices.**
  Keep what maintenance needs (per-key sums+counts, multisets) in the state.
  If a rescan is unavoidable, make it one set-oriented query (one semi-join),
  not chunked IN-lists per key (3.5M rows per RF: ~50 s chunked, 5.6 s as one
  semi-join).
- **Cost scales with delta size plus a small per-commit constant.** Calibrate
  and validate cost models on bulk inserts/deletes spread over keys. Fitting
  on tiny UPDATEs gave 5.6 ms/row and made the policy pick `RebuildAll` (5.4
  s) over `ApplyDelta` (52 ms) for an RF-sized delta.
- **Always go through `services.ChooseMaintenance`** so decisions and costs
  are logged; do not hard-wire the action.
- **Parallelize independent per-partition and per-key maintenance** with
  `core::ParallelEach` / `ParallelReduce` under the maintenance cap
  (`engine_interfaces.md` for the fork-join rules: engine lookups first and
  serially, slices own disjoint keys). Other accelerators' maintenance already
  runs in parallel at commit, under the same cap. A serial per-row loop over a
  refresh is what dominates at large scale factors.
