# Stage 3: Optimization analysis (iter {{optimization_iter}} / {{optimization_max_iterations}})

**{{display_name}} Q{{query_number}}** — choose exactly **one** evidence-backed, falsifiable hypothesis.

## Structural priority tiers (organizing context — not a forced order)

1. eliminate unnecessary work
2. exploit selectivity
3. reduce decoding/materialization
4. scan/operator fusion
5. specialize vector/data-layout paths
6. improve hash/aggregation state
7. low-level micro-optimizations

## Task

Read the **context packet** in the user message (measurements + tier-organized evidence).

For **base-kernel** workloads: use the context packet excerpts; read source files only when a specific symbol needs verification.

For **aux-state** workloads: read the full aux implementation (`03_optimization_analysis.md` workflow) before choosing a hypothesis.

Output **one** hypothesis as JSON:

```json
{
  "bottleneck": "short name",
  "tier": "one of the tier ids above",
  "target_metrics": ["metric ids from evidence"],
  "why": "why this should help, citing evidence",
  "upper_bound_pct": null,
  "falsifiable_prediction": "what measurement should improve if correct",
  "grep_targets": ["path:symbol hints for patch agent"]
}
```

- Set `upper_bound_pct` only when directly justified by measured runtime fractions (Amdahl-style). Otherwise `null`.
- End with `HYPOTHESIS_COMPLETE`.

Do not propose patches in this stage.

{{benchmark_notes}}

{{constraints}}
