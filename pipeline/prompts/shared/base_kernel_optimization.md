# Base single-threaded kernel optimization (no aux-state)

Goal: **beat** DuckDB reference SQL on the benchmark dataset (`PRAGMA threads=1`, full result consumption).

## Workflow

1. Read profile counters and phase timings in `optimization_runtime.md`.
2. Form a **hot-path hypothesis** from dominant phase + counter ratios.
3. Pick a **coordinated bundle** (2–4 related changes) from the strategy palette — not isolated tweaks.
4. **Verify** by reading the production path (fused prescan → consume → aggregate).
5. **A/B test** the bundle; keep only what improves `speedup_median` and relevant counters.

When speedup is far below 1.0x, prioritize structural changes (scan order, filter pushdown, build-phase interning, vector fast paths) over micro-opts.

## Scale factors (SF1 iteration → SF20+ production)

The pipeline profiles and gates primarily on **SF1** for speed. Final TPC-H benchmarking targets **SF20 or higher**. An optimization that only works at SF1 is a regression.

**Before accepting a bundle, verify:**
- Capacities come from bind-time `row_estimate` / table counts — not SF1 profile literals (`150000`, `6000000`, etc.).
- Aggregation structures scale with **qualifying** keys/rows, not fixed SF1 maxima.
- No full-domain finalize scans unless the domain is provably bounded at bind time for any scale factor.

The `sf10` gate is a coarse scale check when `tpch_sf10` is configured — treat failures as scaling bugs. See `scale_factor_contract.md`.

## Useful counter ratios

| Ratio | What it tells you |
|-------|-------------------|
| `pre_rows / qualifying_rows` | Scan amplification — rows touched vs rows that aggregate |
| `generic_fallback_rows / pre_rows` | Whether vector fast paths are active |
| `num_aggregate_us / num_scan_us` | How much of the num pass is consume (may include rejected rows) |

Cross-check reference SQL and `analysis.json` for join order, filters, and GROUP BY keys.

## Anti-patterns

- One-line tweaks when the gap to DuckDB is large (need coordinated bundles)
- Optimizing aggregation because a phase label says "scan" without tracing the inner loop
- Profiling a different path than production (chunk path vs fused scan)
- `getenv` on hot paths; SF-hardcoded capacities
- SF1-only wins that add full-domain scans or cardinality constants from the iteration dataset
