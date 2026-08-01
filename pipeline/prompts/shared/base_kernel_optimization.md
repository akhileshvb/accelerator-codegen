# Base single-threaded kernel optimization (no aux-state)

Goal: **beat** DuckDB reference SQL on the benchmark dataset (`PRAGMA threads=1`, full result consumption).

## Workflow

1. Read profile counters and phase timings in `optimization_runtime.md`.
2. Form a **hot-path hypothesis** from dominant phase + counter ratios.
3. Pick a **coordinated bundle** (2–4 related changes) from the strategy palette — not isolated tweaks.
4. **Verify** with `grep_context` on the production path (fused prescan → consume → aggregate).
5. **A/B test** the bundle; keep only what improves `speedup_median` and relevant counters.

When speedup is far below 1.0x, prioritize structural changes (scan order, filter pushdown, build-phase interning, vector fast paths) over micro-opts.

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
