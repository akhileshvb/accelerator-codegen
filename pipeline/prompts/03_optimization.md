# Stage 3: Optimization (iteration {{optimization_iter}})

Optimize **Q{{query_number}}** single-threaded accelerator. One bounded change per iteration.

## Context

{{retrieval_brief}}

## Baseline

{{baseline_summary_json}}

## Last experiments

{{experiment_log_excerpt}}

## Task

1. State one hypothesis targeting a measured bottleneck.
2. Apply exactly one change via `apply_patch` (compile-time guard if experimental).
3. `run_command`: build + `ctest -R tpch_q{{query_number}}`.
4. `request_gate`: correctness → baseline → sf10 (if state-sizing) → profile.
5. Write `playbook_proposal.md` if retain or revert with evidence.

Do not stack unmeasured changes.

{{handbook_excerpt}}

{{constraints}}
