# Stage 4: Audit

Audit **Q{{query_number}}** accelerator for benchmark fairness, **scale safety**, and production-path correctness.

{{hard_guidelines}}

## Context

Experiment log (tail): see `{{artifacts_path}}/experiment_log.md`

Gate summaries on disk: `{{artifacts_path}}/gates/` — read `baseline.json`, `profile.json`, `fairness.json` on demand.

## Task

1. Review kernel, package, tests, benchmark for fairness violations (`grep_context` preferred over full kernel reads).
2. Check hardcoded capacities, hidden work, production `getenv`, and iteration-scale-only assumptions (see `scale_factor_contract.md`).
3. Confirm native base scans and no answer-caching disguised as aux state.
4. Fused kernels: `pre_scan_us` may include aggregation unless `num_aggregate_us` is separate — check aggregation share before blaming scan.
5. Aux: rebuild paths must resync, not invalidate-only, when rebuild is selected.
6. `request_gate` with `fairness`{{audit_gate_suffix}}.
7. Check `hard_guidelines.md` §4 (exact for any parameter value incl. NULL, off-spec data, 64-bit keys) where it applies to this kernel.
8. Write `{{artifacts_path}}/final_audit.md` (10-section report).

Read-only code changes only for clear fairness bugs.

{{constraints}}
