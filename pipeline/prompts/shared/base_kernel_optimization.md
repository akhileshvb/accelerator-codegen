# Stateless kernel optimization

The kernel beats the engine's own reference SQL on the benchmark dataset (reference limited to the same thread budget, full result consumption). The how-to for the inner loops is `low_level_execution.md`; this file is the loop around it.

## Workflow

1. Read the profile counters and phase timings.
2. Name the hot path from the dominant phase and the counter ratios.
3. Pick a **coordinated bundle** (2-4 related changes) from: scan order and pushdown, build-time interning to dense ids, fused passes, SIMD inner loops, branch-free selection, prefetch. Not isolated tweaks.
4. Trace the production path (fused prescan, consume, aggregate) to confirm the bundle hits the loop the profile named.
5. A/B the bundle. Keep it only if `speedup_median` and the relevant counters improve.

While the speedup is far below 1.0x, make structural changes (scan order, filter pushdown, interning, vector fast paths) before micro-optimizations.

## Scale check before keeping a bundle

Does it still win with fact-table rows x3000 and more distinct keys? Capacities come from bind-time `row_estimate` and table counts, never SF1 literals. Aggregation structures scale with qualifying keys, not fixed maxima. No full-domain finalize unless the domain is bounded at bind time. A failing `sf10` gate (when configured) is a scaling bug (`scale_factor_contract.md`).

## Counter ratios

| Ratio | What it tells you |
|-------|-------------------|
| `pre_rows / qualifying_rows` | Scan amplification: rows touched vs rows that aggregate |
| `generic_fallback_rows / pre_rows` | Whether the vector fast paths are active |
| `num_aggregate_us / num_scan_us` | Share of the scan pass that is consume (may include rejected rows) |

## Anti-patterns

- One-line tweaks while the gap to the reference is large.
- Optimizing aggregation because a phase label says "scan", without tracing the inner loop.
- Profiling a different path than production (chunk path vs fused scan).
- `getenv` on hot paths; SF1-hardcoded capacities or full-domain scans.
