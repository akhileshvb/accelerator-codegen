# Scale factor contract (iteration vs production)

The pipeline optimizes on **small datasets for fast feedback**. Production benchmarking targets **much larger scale factors**. Designs must remain correct and efficient when data grows — not only on the iteration database.

## TPC-H tiers

| Tier | Typical DB | When used | Agent obligation |
|------|------------|-----------|------------------|
| **Iteration (SF1)** | `ACCEL_TPCH_DB` / `tpch_sf1` | Correctness, baseline, profile, aux write-freq sweeps, A/B quick benchmarks | Primary optimization signal during the pipeline — fast turnarounds |
| **Scale check (SF10)** | `scale_db` / `tpch_sf10` | `sf10` gate after optimization + audit | Catches sizing blowups, state that only fits SF1, benchmark crashes at 10× rows |
| **Production benchmarks (SF20+)** | optional `tpch_sf20` (manual / out-of-pipeline) | Final validated benchmarking outside rapid iteration | Speedups must hold here |
| **Design target (SF3000, ~8 TB RAM)** | not materialized; approximated by the magnitude databases below | Sizing of every key, offset, count, sum, shard and buffer | Fact table ~2e10 rows, ids to ~2e10 (sparse, beyond 2^32), 4 GB+ heaps, sums beyond 2^63, refreshes of 10^5-10^7 rows per commit; all state in memory |

**Rule:** A technique that wins on SF1 but uses SF1-hardcoded capacities, full-domain scans sized for SF1, or structures with superlinear growth in table cardinality is **not acceptable**, even if SF1 gates pass. Agents must design for **high speedups at high SF**, not merely “doesn’t crash.”

## SEC-EDGAR

SEC workloads use **SF3** as both iteration and production scale in this repo (`ACCEL_SEC_EDGAR_DB`). There is no separate SF1/SF20 split — but the same rules apply: size from bind-time `row_estimate` / table counts, never from a one-off profile snapshot.

## Sizing rules (all benchmarks)

1. **Bind-time sizing** — derive capacities from `row_estimate`, `GetNativeScanTableRowCount`, or query-specific bind parameters. Never hardcode `150000`, `6000000`, or other SF1 cardinalities in production paths.
2. **Sparse / dense tradeoffs** — prefer dense direct indexing only when the key domain is bounded by bind metadata *and* keys are known non-negative and small; use chunked/sparse structures (`AnyKeyCowArray`, `FlatHashMap`, ...) when distinct counts scale with data or keys can be large/negative. Keys are 64-bit; no fixed-width packing, no fixed dense caps (see `hard_guidelines.md` §4).
3. **Avoid full-domain finalization** — do not iterate fixed slots sized for SF1 maxima when qualifying groups are sparse at SF20.
4. **Aux-state partitions** — partition count and per-partition payload must scale with query window and mutation locality, not total SF1 table size. Read paths must stay bounded to the query window at any scale.
5. **Maintenance locality** — bulk refresh and point writes should scale with touched data / local partitions, not always full rebuilds.
6. **No production `getenv`** — scale selection comes from bind context and table statistics, not environment variables on the hot path.
7. **No final-answer aux** — do not store precomputed final query results as aux state; redesign intermediates if needed when hitting a ceiling.

## Large-scale failure classes (design against each; audit and tests check each)

Each row is a way a design that passes at SF1-SF20 breaks or stops being incremental at SF3000. `substrate_primitives.md` lists the tools; `hard_guidelines.md` §4 holds the exactness rules these build on.

| Failure class | The module must | How it is checked |
|---|---|---|
| Sums overflow 64 bits (scaled decimals, products of them, running totals, squares) | Accumulate, store, delta and emit in 128-bit (`ScaledDecimal128`, overflow-checked); widen before multiplying; never narrow to 64 bits without a checked cast; thresholds compared exactly | `tpch_big` database (money columns x3000): differential vs SQL for build, delta and rebuild paths |
| Keys, ids, offsets or counts exceed 32 bits (row ids, arena / blob / dictionary offsets, per-key counters, partition ids, loop indices over rows, hash-combined ids) | Use 64-bit for all of them; a narrowing that can lose information is a checked cast that fails loudly, never wraps, skips the row or falls to per-row SQL | `tpch_shift` database (every key shifted to its SF3000 maximum): differential incl. writes of such keys |
| Arrays or tables sized by the maximum key, or dense ranges over a sparse key space | Size by distinct or touched keys (paged / sparse COW containers); a dense layout only when the domain is bounded by table statistics, with a sparse fallback beyond its bound, not a skip | `tpch_shift`: state bytes and RSS track row counts, not the maximum key |
| Fixed shard, slot or partition counts | Derive counts from expected rows (`DataScaleRows()`, bind-time row estimate) so entries per unit stay bounded; per-unit costs (clone, sort, memmove, index merge) must not grow with total rows | Per-changed-row maintenance time and per-key read time flat from SF1 to SF20 |
| A hard-coded cap that forces `RebuildAll` or a flatten outside the cost model | No `kMax...` guard that rebuilds; growth is absorbed incrementally. Every rebuild is a priced candidate of `ChooseMaintenance` with honest `rows_touched`, `affected_partition_count` and `partition_rebuild_supported`; a model records `calibrated_scale_rows` | RF-sized commits on the magnitude databases: the decision log shows `ApplyDelta`, `ReadAuxExecuteCounts` shows no rebuild or fallback |
| Overlays, tombstones or delta logs that only grow | Fold into the base incrementally (work proportional to recent changes, per touched unit); a delete removes its entry | State size and read time after many insert/delete cycles return to the fresh-build baseline |
| Per-commit work or COW clones proportional to total state rather than touched keys (whole-payload copies, a clone of a whole shard per touched key, a per-commit base-table rescan or full-state pass) | Clone per touched unit at a cost proportional to the rows changed in it; keep in state what maintenance needs instead of re-reading base tables; batch lookups into one set-oriented query | Maintenance time and bytes copied per changed row at SF1 vs SF20 vs magnitude databases |
| Serial per-row work dominating at large refresh or build sizes | Split the per-row loop over the phase cap (`ParallelEach` / `ParallelReduce` in maintenance after serial engine lookups; `PartitionedBuild` in `Build`; `ParallelReduce` in `Execute`), results identical at any cap | 1-thread vs N-thread differential; maintenance and build time fall with the caps |
| Transient memory peaks (a whole-table vector then a copy, partials all alive at merge, per-commit buffers sized by state, payloads held past the query) | Stream into the final structure; partials are cheap and merge by moving; commit buffers sized by the delta; hold a payload `shared_ptr` only for the query | Peak RSS of a build and of an RF-sized commit vs settled state size (a small multiple, not 2-5x) |
| Node-based maps, per-row strings, scans of all slots or groups | Columnar `accelerators/prims/` primitives; reads bounded to the query window | bytes/row estimate; reads flat in total state |

Verification set for every accelerator (in addition to the SF1 gates; none of this is a gate yet, so run it and report it):

1. `scripts/make_tpch_magnitude_dbs.sh <tpch_sf1.duckdb> <dir>` once, then the shared differential (`test_tpch_shared_differential`, and the engine's equivalent) with `ACCEL_TPCH_DB` or the path argument on `tpch_shift.duckdb` and `tpch_big.duckdb`: reads, RF-shaped bulk writes and point writes.
2. The same differential at `ACCEL_AUX_READ_THREADS` / `BUILD_THREADS` / `MAINT_THREADS` = 1 and a higher value. A phase below its grain runs as one slice; set `ACCEL_AUX_TEST_GRAIN=1` (test-only cap on every slice grain) so even one-row commits and small reads split across threads, and the results must be identical.
3. `ReadAuxExecuteCounts` and the maintenance decision log across the writes: no unexpected rebuild, no fallback.

## Optimization workflow implication

1. Use SF1 profile + gates to choose **structure** (scan order, interning, fused path, aux state family).
2. Before accepting a bundle, ask: **does this still deliver a large speedup if lineitem rows ×20 and distinct keys grow?**
3. Prefer changes that improve SF1 **and** remove SF1-specific constants or full-domain work.
4. If SF1 read-path gains stall, redesign aux structure for cardinality/shape — still without caching final answers.
5. When the `sf10` gate is available, treat a crash, timeout, or sharp regression as a scaling bug — not an optional check.

## Manual SF20+ validation (outside pipeline)

When a `tpch_sf20` (or higher) DuckDB file is available:

```bash
ACCEL_TPCH_DB=/path/to/tpch_sf20.duckdb ./build-release/benchmark_tpch_q{N}
```

The pipeline does not run SF20 gates by default (runtime cost). Agents must still design as if it will.
