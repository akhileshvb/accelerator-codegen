# Stage 4: Aux-state audit

Audit **Q{{query_number}}** aux accelerator (`{{aux_function_name}}`).

## Context

{{analysis_json}}

## Task

1. Verify `correctness_facts` match implementation (partitioning, mutation interpreter, state payload).
2. Confirm `mutation_coverage` behaviors are exercised in tests (each non-`supported_delta` at least once).
3. Verify fallback path produces correct results vs base kernel.
4. Run quantitative speedup verification:
   - `request_gate`: aux_speedup_verification
   - Pass when `speedup_vs_base = wall_time_base / wall_time_aux >= min_speedup_vs_base`, **or** work reduction meets threshold, **and** claimed operators are eliminated on fast path.
5. Write **`pipeline/artifacts/q{{query_number}}/audit_result.json`** (not `analysis.json`) with measured results:

```json
{
  "design_success": true,
  "speedup_verification": {
    "status": "pass|fail|skipped_unsuccessful_design",
    "wall_time_base_us": 0,
    "wall_time_aux_us": 0,
    "speedup_vs_base": 0.0,
    "work_reduction_pct": 0.0,
    "operators_eliminated_confirmed": true,
    "notes": ""
  }
}
```

6. Run remaining gates: aux_correctness, aux_unit, fairness.

If speedup verification fails, set `design_success: false` unless workload allows unsuccessful designs.

{{agent_boundary}}

{{constraints}}

{{mutation_coverage}}
