# Stage 3: Aux-state optimization analysis (iter {{optimization_iter}})

**{{display_name}} Q{{query_number}}** — choose exactly **one** optimization hypothesis that **aggressively improves read-path performance** for this aux-state implementation.

## Mindset

Think like a performance engineer optimizing a hot query path at production scale:

1. **Understand the implementation** — read the full aux module before deciding (see step 1 below).
2. **Attack the read hot path** — `EmitResult`, order/lineitem lookup, state decode, residual scans, hash/map chasing. Maintenance must stay correct but **read latency is the score**.
3. **Pick one falsifiable change** — structural read-path wins preferred over micro-opts; must **scale with SF** (bind-time `row_estimate`, amortized growth — no SF1 constants).

**Optimization score** = `read_only_query_speedup` (aux hit vs base kernel at 0% writes). DuckDB-reference workload speedup and cold build time are **not** the optimization target.

## Workflow (do this first)

Use `read_file` to load the **full** editable implementation (use `limit=600` per file):

- `{{kernel_path}}` (module.cpp)
- `{{header_path}}` (module.hpp)
- `{{package_path}}` (finalize.cpp)
- `{{cost_model_config}}`

Do **not** edit files in this stage. Do **not** read `src/generated/tpch/q{{query_number}}/base/**` unless you need read-only context.

## Structural priority tiers (read-path — prefer higher tiers)

1. **eliminate residual full-table / full-domain scans** on the query path (orders, lineitem, aux cold paths)
2. **replace map/hash indirection** with dense arrays, SOA layouts, or direct indexing where cardinality is bounded by `row_estimate`
3. **exploit selectivity** — prune early, filter before decode, skip untouched partitions
4. **reduce decode/materialization** — fewer copies, lazy fields, vectorized gather
5. **scan/operator fusion** — combine passes, avoid intermediate vectors
6. **specialize vector/data-layout paths** — SIMD-friendly layouts, cache-line alignment
7. **low-level micro-optimizations** — only after higher tiers are exhausted

## Output

After reading the implementation and the context packet (measurements + evidence), output **one** hypothesis as JSON:

```json
{
  "bottleneck": "short name",
  "tier": "one of the tier ids above",
  "target_metrics": ["read_only_query_speedup", "accel_hit_median_us"],
  "why": "why this fits this aux module and should improve read latency at scale, citing evidence and code structure",
  "upper_bound_pct": null,
  "falsifiable_prediction": "what read_only_query_speedup or accel_hit_median_us should improve if correct",
  "grep_targets": ["path:symbol hints for patch agent"],
  "edit_plan": ["ordered list of minimal file edits the patch agent should make"]
}
```

- Set `upper_bound_pct` only when directly justified by measured runtime fractions (Amdahl-style). Otherwise `null`.
- Reject hypotheses that only help at SF1 via hardcoded capacities or cardinality literals.
- End with `HYPOTHESIS_COMPLETE`.

The patch agent will read the same files, apply `edit_plan`, and hand off to the orchestrator for build → smoke → benchmark → accept/reject.

{{benchmark_notes}}

{{constraints}}
