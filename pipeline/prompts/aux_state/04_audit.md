# Stage 4: Aux-state audit

Audit **Q{{query_number}}** (`{{aux_function_name}}`).

{{hard_guidelines}}

## Audit contract

```json
{{audit_context_json}}
```

Full analysis: `{{full_analysis_path}}`

## Task

Hard gates after this stage verify **correctness** and run a **fair SF1 bench sweep**. Your job is analysis + a short written audit — not brittle micro-gates.

1. **Correctness / contract** — implementation matches the selected design (partitioning, mutations, payload, `cross_accelerator_concurrency`). Confirm `mutation_coverage` behaviors are tested.
2. **Sweep evidence** — read `{{artifacts_path}}/aux_bench_summary.json` and/or `gates/aux_bench_sweep.json` (or `request_gate` `aux_bench_sweep` if missing). Record 0% / write-mix speedups and hit latency.
3. **Scale review** (write this up; do not invent SF10 numbers if unmeasured):
   - Do structures size from bind-time / `row_estimate` (or equivalent), or are there SF1-hardcoded capacities / loops?
   - Per-query and per-write cost: O(touched work) vs full rebuild / full-table scan on the hot path?
   - Any absolute SF1 cardinalities (`150000` / `600000` / `800000`) used as live capacities in **aux** code?
4. **Harness / benchmark fairness** — confirm the timed path is the real production query (consume results; no result-cache-as-aux; no skipping work the base path does). Flag any harness gaming.
5. Write `{{artifacts_path}}/audit_result.json` and `{{artifacts_path}}/final_audit.md` with clear verdicts:

```json
{
  "design_success": true,
  "correctness_ok": true,
  "bench_sweep": {
    "read_only_query_speedup": 0.0,
    "write_mix_workload_speedup": 0.0,
    "write_mix_pct": 10,
    "accel_hit_median_us": 0.0,
    "notes": ""
  },
  "scale_review": {
    "likely_scales": true,
    "risks": [],
    "notes": ""
  },
  "fairness_review": {
    "cheating_suspected": false,
    "issues": [],
    "notes": ""
  }
}
```

Optional: `request_gate` `fairness` is a **heuristic** static scan (informational). Do not treat substring false-positives as blockers; explain them in the audit.

Post-stage hard gates: `aux_correctness`, `aux_unit`, `aux_mutation_scenarios`, `aux_bench_sweep`.

{{shared_docs_reference}}

{{constraints}}
