# Stage 4: Aux-state audit

Audit **Q{{query_number}}** (`{{aux_function_name}}`).

## Audit contract

```json
{{audit_context_json}}
```

Full analysis: `{{full_analysis_path}}`

## Task

1. Verify implementation matches contract (partitioning, mutations, payload, `cross_accelerator_concurrency`).
2. Confirm `mutation_coverage` behaviors are tested (read `mutation_coverage.md` if needed).
3. `request_gate`: **aux_speedup_verification** only — capture wall times for `audit_result.json`.
4. Write `{{artifacts_path}}/audit_result.json`:

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

Remaining gates (`aux_correctness`, `aux_unit`, `aux_cost_model_calibrate`, `aux_multi_connection`, `aux_cross_accel`, `fairness`) run automatically after this stage.

{{shared_docs_reference}}

{{constraints}}
