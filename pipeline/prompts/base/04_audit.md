# Stage 4: Audit

Audit **Q{{query_number}}** accelerator for benchmark fairness, **scale safety** (SF3000), production-path correctness and hardware-aware inner loops (`pipeline/prompts/shared/docs/low_level_execution.md`).

{{hard_guidelines}}

## Context

Experiment log (tail): see `{{artifacts_path}}/experiment_log.md`

Gate summaries on disk: `{{artifacts_path}}/gates/` — read `baseline.json`, `profile.json`, `fairness.json` on demand.

The user message carries a deterministic query digest (file map with roles, public entry points, the experiment log, open findings) and the cumulative diff since implementation began: review from those and open a file only where they leave a question open.

## Task

1. Review kernel, package, tests, benchmark for fairness violations (targeted `rg` / `sed -n` reads preferred over full kernel reads).
2. Check hardcoded capacities, hidden work, production `getenv`, and iteration-scale-only assumptions (see `scale_factor_contract.md`).
3. Confirm native base scans, no persistent state, and no answer caching.
4. Fused kernels: `pre_scan_us` may include aggregation unless `num_aggregate_us` is separate — check aggregation share before blaming scan.
5. Hot loops: report a scalar loop over data that a vector path would speed up, per-row allocation, and branchy predicates, as risks with file:line.
6. `request_gate` with `fairness`{{audit_gate_suffix}}.
7. Check `hard_guidelines.md` §4 (exact for any parameter value incl. NULL, off-spec data, 64-bit keys) where it applies to this kernel, and that the frozen suite (`tests/workload/`: `params.sql`, `domain.yaml`, `suite.yaml`) covers every parameter of the query; list gaps as findings.
8. Write `{{artifacts_path}}/final_audit.md` (10-section report).

Read-only code changes only for clear fairness bugs.

{{constraints}}
