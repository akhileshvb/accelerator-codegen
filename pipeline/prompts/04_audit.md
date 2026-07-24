# Stage 4: Audit

Audit **Q{{query_number}}** accelerator for benchmark fairness and scale safety.

## Context

{{experiment_summary}}

{{baseline_summary_json}}

## Task

1. Review kernel, package, tests, benchmark for fairness violations.
2. Check for SF1-hardcoded capacities, hidden work, production `getenv` usage.
3. Run `request_gate` with `fairness` and `sf10`.
4. Write `pipeline/artifacts/q{{query_number}}/final_audit.md` (10-section report per optimization template).

Read-only code changes only for clear fairness bugs.

{{constraints}}
