# Stage 2: Aux-state implementation

Implement the aux-state accelerator for TPC-H **Q{{query_number}}**.

## Context

{{analysis_json}}

## Query

```sql
{{reference_sql}}
```

## Instructions

- Implement `files_to_create` from analysis using `module_layout` conventions.
- Implement the **selected** candidate from `state_candidate_search` (`correctness_facts.state_design.selected_candidate_id`).
- Treat `correctness_facts` as the semantic contract.
- Treat `performance_hypotheses` as initial calibration targets only.
- If `fitness_assessment.recommended` is false, stop unless workload override allows it.

## Registration

{{registration_checklist}}

## Substrate APIs

{{substrate_contract}}

After implementation:

1. Build and run `ctest -R tpch_q{{query_number}}_aux` and `ctest -R q{{query_number}}_`.
2. The orchestrator runs `aux_bench_sweep` (write frequency 0%, 1%, 10%, 50%) as the baseline performance signal.
3. Record any implementation notes in `experiment_log.md`; bench results land in `aux_bench_summary.json` and `optimization_state.json`.

{{agent_boundary}}

{{constraints}}

{{profiling_conventions}}
