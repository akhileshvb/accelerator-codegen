# Stage 3: Aux-state optimization (iteration {{optimization_iter}})

Tune **performance only** for **Q{{query_number}}**. **One bounded change** per iteration.

## Context

{{retrieval_brief}}

Full analysis (candidate switches): `{{full_analysis_path}}`

Composite score = min workload speedup (0/1/10/50% writes) + 0.1 × read-only query speedup. Details: `bench_sweep_contract.md`.

## Allowed changes (exactly one)

**A. Cost-model / maintenance tuning** — coefficients (including `interference_*` in cost model JSON), eligibility thresholds, EWMA, maintenance-path code (preserve mutation semantics). Log `change_kind: cost_model` or `maintenance_code`.

**B. Candidate switch** — viable alternate from retrieval brief; read full `analysis.json`; update `correctness_facts.state_design`. Log `change_kind: candidate_switch`.

**C. Structural re-analysis** — last resort; call `request_structural_reanalysis` with `bottleneck_type`, evidence, `proposed_direction`.

Not allowed without re-analysis: dependent tables, legal delta semantics, mutation_coverage behavior changes.

## Task

1. One hypothesis tied to `composite_score` or a weak write rate.
2. One `apply_patch` change.
3. `run_command`: build + `ctest -R tpch_q{{query_number}}_aux` + `ctest -R q{{query_number}}_`.
4. Append to `experiment_log.md`: change_kind, hypothesis, expected effect.

**Official gates run after this stage** (do not call `request_gate`). Results appear in the next iteration's retrieval brief.

Stops after **2 iterations** without improving `composite_score`.

{{shared_docs_reference}}

{{constraints}}
