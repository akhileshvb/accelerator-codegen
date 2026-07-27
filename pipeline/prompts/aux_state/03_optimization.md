# Stage 3: Aux-state optimization (iteration {{optimization_iter}})

Tune **performance only** for **Q{{query_number}}** aux accelerator. One bounded change per iteration.

## Context

{{retrieval_brief}}

## Analysis (hypotheses + candidate search)

{{analysis_json}}

## Bench sweep signal (primary feedback)

The orchestrator runs `aux_bench_sweep` after each iteration: point writes at **0%, 1%, 10%, and 50%** frequency.

```json
{{aux_bench_summary_json}}
```

```json
{{optimization_state_json}}
```

**Composite score** = min workload speedup across write frequencies + 0.1 × read-only (0%) query speedup. Higher is better. Compare against `best_composite_score` in optimization state.

## Last experiments

{{experiment_log_excerpt}}

## Allowed changes (pick exactly one per iteration)

### A. Cost-model / runtime tuning (low risk)

- Cost-model coefficients (`config/q{{query_number}}_aux_cost_model.json`)
- Eligibility thresholds that do **not** change legal mutations
- EWMA / confidence tuning
- Performance guards (e.g. skip delta when cost exceeds margin)

### B. Alternate state candidate (higher risk, allowed when analysis listed ≥3 viable candidates)

Switch `active_candidate_id` to another **viable** entry in `state_candidate_search.candidates` that was not yet implemented:

1. Copy that candidate's grain, organization, composition, and read-path fields into `correctness_facts.state_design` (update `selected_candidate_id`).
2. Re-implement aux kernels / maintenance to match the alternate design (partition layout, payload, read path).
3. Do **not** edit the candidate list itself or re-run analysis unless semantics truly change.

Log `change_kind: cost_model` or `change_kind: candidate_switch` in `experiment_log.md`.

## Not allowed without re-analysis

- Dependent tables / watched columns
- Legal delta vs rebuild vs invalidate semantics (unless implied by the chosen candidate)
- `mutation_coverage` behavior changes outside what the candidate requires

If a semantic gap is found, write `analysis_revision_request.md` and re-run analysis.

## Task

1. Review bench sweep by frequency and `optimization_state_json`. State one hypothesis (cost model vs alternate candidate).
2. Apply exactly one change via `apply_patch`.
3. `run_command`: build + `ctest -R tpch_q{{query_number}}_aux` + `ctest -R q{{query_number}}_`.
4. `request_gate`: aux_correctness → aux_unit → aux_bench_sweep.
5. Compare new `composite_score` to prior best; log results in `experiment_log.md`.

{{constraints}}

{{maintenance_framework}}

{{runtime_policy}}
