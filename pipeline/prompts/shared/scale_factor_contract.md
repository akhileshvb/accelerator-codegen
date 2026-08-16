# Scale factor contract (iteration vs production)

The pipeline optimizes on **small datasets for fast feedback**. Production benchmarking targets **much larger scale factors**. Designs must remain correct and efficient when data grows — not only on the iteration database.

## TPC-H tiers

| Tier | Typical DB | When used | Agent obligation |
|------|------------|-----------|------------------|
| **Iteration (SF1)** | `ACCEL_TPCH_DB` / `tpch_sf1` | Correctness, baseline, profile, aux write-freq sweeps, A/B quick benchmarks | Primary optimization signal during the pipeline — fast turnarounds |
| **Scale check (SF10)** | `scale_db` / `tpch_sf10` | `sf10` gate after optimization + audit | Catches sizing blowups, state that only fits SF1, benchmark crashes at 10× rows |
| **Production target (SF20+)** | optional `tpch_sf20` (manual / out-of-pipeline) | Final validated benchmarking outside rapid iteration | **Design target** — algorithms and memory must scale here even though the pipeline rarely runs it |

**Rule:** A technique that wins on SF1 but uses SF1-hardcoded capacities, full-domain scans sized for SF1, or structures with superlinear growth in table cardinality is **not acceptable**, even if SF1 gates pass. Agents must design for **high speedups at high SF**, not merely “doesn’t crash.”

## SEC-EDGAR

SEC workloads use **SF3** as both iteration and production scale in this repo (`ACCEL_SEC_EDGAR_DB`). There is no separate SF1/SF20 split — but the same rules apply: size from bind-time `row_estimate` / table counts, never from a one-off profile snapshot.

## Sizing rules (all benchmarks)

1. **Bind-time sizing** — derive capacities from `row_estimate`, `GetNativeScanTableRowCount`, or query-specific bind parameters. Never hardcode `150000`, `6000000`, or other SF1 cardinalities in production paths.
2. **Sparse / dense tradeoffs** — prefer dense direct indexing only when the key domain is bounded by bind metadata; use hash maps or two-level structures when distinct counts scale with data.
3. **Avoid full-domain finalization** — do not iterate fixed slots sized for SF1 maxima when qualifying groups are sparse at SF20.
4. **Aux-state partitions** — partition count and per-partition payload must scale with query window and mutation locality, not total SF1 table size. Read paths must stay bounded to the query window at any scale.
5. **Maintenance locality** — bulk refresh and point writes should scale with touched data / local partitions, not always full rebuilds.
6. **No production `getenv`** — scale selection comes from bind context and table statistics, not environment variables on the hot path.
7. **No final-answer aux** — do not store precomputed final query results as aux state; redesign intermediates if needed when hitting a ceiling.

## What SF1 wins can hide

| SF1-only win | Fails at SF20 because |
|--------------|----------------------|
| `std::vector` sized to SF1 max groups | Memory and cache blow up |
| Per-row `unordered_map<std::string,…>` | Probe cost dominates at scale |
| Scan all partition slots / all groups | Read cost ∝ total state, not query window |
| Micro-opts on cold paths | Noise at SF1; dominant cost shifts to scan/aggregate at scale |
| Full rebuild on every point write | Write amplification destroys mixed workloads at scale |

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
