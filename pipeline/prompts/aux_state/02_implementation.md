# Stage 2: Aux-state implementation

Implement the aux-state accelerator for TPC-H **Q{{query_number}}**.

## Implementation contract

```json
{{implementation_context_json}}
```

Full analysis: `{{full_analysis_path}}` · Workload SQL: `{{reference_sql_path}}`

## Instructions

- Implement `files_to_create`; selected candidate = `correctness_facts.state_design.selected_candidate_id`.
- `correctness_facts` is the semantic contract.
- Read `module_layout.md`, `read_path_scaffold.md`, `bench_sweep_contract.md`, `concurrency_contract.md` on demand (see shared docs below).

## Benchmark & concurrency

- `benchmark_q{{query_number}}_aux.cpp`: full write-freq sweep harness per `bench_sweep_contract.md` (port from `benchmark_q14_aux.cpp` or `benchmark_q1_aux.cpp`).
- Concurrency: irrelevant mutations → NoOp; bounded window reads; per-accelerator epochs; reader epoch pin + execution gate (`read_path_scaffold.md`).

**Official gates run after this stage** (do not call `request_gate`): `aux_correctness`, `aux_unit`, `aux_cost_model_calibrate`, `aux_multi_connection`, `aux_cross_accel`, `aux_bench_sweep`.

Wire `EstimateInterferenceUs` in `*_cost_formulas.cpp`; `config/q{{query_number}}_aux_cost_model.json` is calibrated by the gate.

## Before finishing

1. `run_command`: build + `ctest -R tpch_q{{query_number}}_aux` + `ctest -R q{{query_number}}_`.
2. Build `benchmark_tpch_q{{query_number}}_aux`.

## Structural re-analysis (rare)

If a structural bottleneck cannot be fixed by viable alternates in `analysis.json`, call `request_structural_reanalysis` with evidence (max **2** per pipeline run).

{{shared_docs_reference}}

{{agent_boundary}}

{{constraints}}
