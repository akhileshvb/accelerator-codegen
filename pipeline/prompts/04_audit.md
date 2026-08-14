# Stage 4: Audit

Audit **Q{{query_number}}** accelerator for benchmark fairness, **scale safety (SF1 → SF20+)**, and production-path correctness.

## Context

Experiment log (tail): see `{{artifacts_path}}/experiment_log.md`

Gate summaries on disk: `{{artifacts_path}}/gates/` — read `baseline.json`, `profile.json`, `fairness.json` on demand.

## Task

1. Review kernel, package, tests, benchmark for fairness violations (`grep_context` preferred over full kernel reads).
2. Check hardcoded capacities, hidden work, production `getenv`, and SF1-only assumptions (see `scale_factor_contract.md`).
3. Fused kernels: `pre_scan_us` may include aggregation unless `num_aggregate_us` is separate — check aggregation share before blaming scan.
4. `request_gate` with `fairness`{{audit_gate_suffix}}.
5. Write `{{artifacts_path}}/final_audit.md` (10-section report).

Read-only code changes only for clear fairness bugs.

{{constraints}}
