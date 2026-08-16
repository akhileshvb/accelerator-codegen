# Hard guidelines (all agent stages)

Non-negotiable for analysis (planning), implementation, optimization, and audit.

## 1. Native base-table storage

Accelerators read **base tables** through the engine’s native storage/scan APIs (`NativeScanSpec` → engine adapter). Do not bypass with SQL-over-client “scans” on the hot path when a native adapter exists.

**Auxiliary state** may and should use **custom storage** (typed in-process images, engine-local aux tables, shared-memory blobs, etc.).

**Forbidden as aux state:** caching or materializing the **final query result** (the exact answer rows for a query binding) and serving it back. Partial aggregates, indexes, eligibility bitmaps, intern tables, membership layouts, and other *intermediate* structures are allowed. If a structure is effectively “precomputed final result for this query,” redesign it.

## 2. Fair benchmarks

No cheating:

- Time the full production query path; consume all results.
- No hidden warmup billed as query latency.
- No SF1-only hardcoded capacities or full-domain work that does not scale.
- No production `getenv` shortcuts that skew timing or skip work.
- Fairness gate is authoritative.

## 3. Aggressive profile-guided optimization that scales

Use measured profile/evidence to drive changes.

### High scale factor is a first-class goal

Iteration benches are often SF1 for speed, but the accelerator must still deliver **strong speedups at high scale factor** (TPC-H **SF10 / SF20+**; SEC at production scale). Treat SF1 wins that blow up memory, scan all groups globally, or hardcode iteration cardinalities as **failures**. Plan with bind-time estimates and data-shape/cardinality reasoning — not SF1 profile snapshots alone.

### When you hit a read-path ceiling

If measured gains stall (micro-opts / emit floors / full-domain filters) and the data shape or cardinality suggests a better family of state:

- You **may redesign the aux-state structure** (partitions, encodings, indexes, dense vs sparse layouts, what is stored vs recomputed).
- Prefer dramatic, scale-aware redesigns over stacking ≤5% tweaks.
- Still forbidden: **precomputed final query results** as aux state (see §1).

### Dual axes: fast reads **and** cheap incremental maintenance

Optimize **both** every attempt — not read-only first with maintenance as an afterthought:

1. **Read axis** — `read_only_query_speedup` / hit latency at 0% writes (weighs more in accept/reject).
2. **Write/maintenance axis** — 10% write-mix `workload_speedup` and point/bulk apply cost from the same quick bench (weighs less, but still gates accepts).

**Accept/reject score** = `read_only^0.75 * write_mix^0.25` (dual-axis weighted geometric mean). A pure read win that deepens the write cliff can lose.

**Maintenance must be cheap:**

- **Bulk / refresh** paths (`Build`, `RebuildAll` / `RebuildPartition`, full resync after cost-model rebuild) — keep them fast, but do **not** use full rebuild as the only response to every point write.
- **Point / synthetic writes** — prefer **ApplyDelta** / touched-key / local-partition updates over `MarkRebuild` + `RebuildAll`.
- When the cost model selects rebuild, **resync/rebuild** so later queries can hit aux again — invalidate-only is not a substitute.
- Prefer maintenance that scales with **touched keys / local partitions**, not full-table rebuilds, when deltas allow.
- Per-attempt quick bench runs **0% reads plus randomly sampled write %s** (fresh sample each attempt): a large drop in write-mix `workload_speedup` (geo-mean across sampled frequencies) is a **first-class failure mode**. If read is already strong and write-mix ≪ 1×, your next hypothesis should attack maintenance cost.

### Read-path ambition

- Expect **large** read-only speedups when aux is valid (often well beyond single-digit percent).
- Prefer structural redesign (layout, encoding, partition pruning, decode elimination) over micro-opts that move <5%.
- Honor exploratory A/B: never ship a change that regresses the **dual-axis** accept score vs best_valid, and do not deepen the write-mix cliff for small read wins.
- Do not game the harness.

## Edit freedom

Edit allowlisted implementation files freely (coordinated refactors OK) under these constraints. Forbidden: hardcoding answers, semantic shortcuts, or otherwise gaming the benchmark.

Temporary **custom profiling** in editable files is encouraged: invent/split/rename phases and counters as needed, gated on `getenv("ACCEL_AUX_CUSTOM_PROFILE")` (must be a no-op when unset) and emitting `ACCEL_CUSTOM_PROFILE` JSON lines for digests. Ad-hoc shell/scripts for measurement are fine.
