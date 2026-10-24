# Stage 1b: Test suite (stateful accelerator)

Write the acceptance test suite of **{{display_name}}** **Q{{query_number}}**. You write tests only: no accelerator, no module code, no gate or harness.

## Goal

**The suite the implementation will be judged by, written before it exists and frozen after you: it must make any wrong answer, stale maintenance or parameter-specialized shortcut visible, at SF3000 shapes and at SF1 speed.**

1. **Parameters across different sets.** `params.sql` holds fixed tuples: in-spec values spread over the whole domain, valid off-spec values (unknown enum, wrong case, partial or empty LIKE patterns, equal or reversed bounds), and edge cases: NULL where the SQL allows it, empty results, ties at a LIMIT cut, extreme dates and amounts, keys beyond 2^32 where a type allows. `domain.yaml` is the domain every run draws **fresh random tuples** from, so a kernel specialized to any fixed values fails; it covers each parameter's whole domain, not just the benchmark generator's.
2. **Every write kind that can change the answer.** `txns/`: point INSERT, UPDATE and DELETE on **each dependent table** (target rows that are in answers, chosen by subquery), bulk RF-shaped chunks, sequences that undo or redo, one aborted transaction (`-- rollback`), keys that move across groups. A write that never changes an answer tests nothing. The schema declares no keys, so a write that duplicates a key is schema-valid: for each key the query joins or groups on, include a transaction that duplicates it (and one that removes the duplicate again). The accelerator must still answer exactly, by handling duplicates or by declining to the SQL fallback while the keys are not unique.
3. **A random write mix.** `write_mix.yaml`: the write-frequency range (percent of operations that are writes) the benchmarks sample from on every run, and the kinds' relative weights as the application makes them. Pick a range an accelerator must stay fast across, not one it was tuned for.
4. **Scale.** `suite.yaml` records the scale scenarios, point-write and bulk-write minimums the gates enforce (`scale_factor_contract.md`).

**Owns:** the query's `tests/workload/` directory, nothing else. **Never touches:** the accelerator, harnesses, gates, `backends/`, `substrate/`, other queries. If a suite already exists there, make it meet this contract; never weaken it. Never record backend or harness requests: if a gate fails for a reason outside `tests/workload/`, call `report_blocker` with the evidence and stop. **Judged by:** the `test_suite_validate` gate (mechanical, below), then, as the implementation's acceptance, `aux_correctness` and `quick_check`.

{{hard_guidelines}}

{{codex_tooling}}

## Inputs

- Analysis: `{{full_analysis_path}}` · compact facts: `{{analysis_json}}`
- Query (`$n` parameters): `workloads/{{benchmark_name}}/q{{query_number}}.sql` · schema: `workloads/{{benchmark_name}}/schema.sql` (its NOT NULL columns are enforced on the test database: a write that breaks the schema is outside what the accelerator is specified for)
- Explore the test database with `backends/{{engine_name}}/host.sh sql`; an existing suite of a neighboring query is a worked example of the format.
- Format, files and the validation rules: `pipeline/prompts/shared/test_suite.md`.

{{change_request}}

## Shared docs (read on demand)

{{shared_docs_reference}}

## Done when

The `test_suite_validate` gate passes (run it with `request_gate`): every tuple and random draw runs the reference SQL; every transaction applies, rolls back cleanly and applies in sequence; every dependent table has insert, update and delete transactions and one that changes an answer; every edge class is covered or waived with a reason; `write_mix.yaml` and `suite.yaml` are well-formed. Then the suite is frozen.
