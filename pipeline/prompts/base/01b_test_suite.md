# Stage 1b: Test suite (stateless kernel)

Write the acceptance test suite of **{{display_name}}** **Q{{query_number}}**. You write tests only: no kernel, no harness, no gate.

## Goal

**The parameter sets the kernel will be judged by, written before it exists and frozen after you: any wrong answer or any specialization to fixed parameter values must show.**

1. `params.sql`: fixed tuples. In-spec values spread over the whole domain, valid off-spec values (unknown enum, wrong case, partial or empty LIKE patterns, equal or reversed bounds), and edge cases: NULL where the SQL allows it, empty results, ties at a LIMIT cut, extreme dates and amounts, keys beyond 2^32 where a type allows.
2. `domain.yaml`: the domain every run draws **fresh random tuples** from, covering each parameter's whole domain.
3. `suite.yaml`: the manifest (rationale, which tuples cover which edge class). The kernel reads no tables it can be written to, so there are no `txns/`.

**Owns:** the query's `tests/workload/` directory, nothing else. **Never touches:** the kernel, harnesses, gates, `backends/`, `substrate/`, other queries. If a suite already exists, make it meet this contract; never weaken it. **Judged by:** the `test_suite_validate` gate (every tuple and random draw runs the reference SQL; every edge class covered or waived with a reason), then, as the kernel's acceptance, the `correctness` gate.

{{hard_guidelines}}


## Inputs

- Query (`$n` parameters): `workloads/{{benchmark_name}}/q{{query_number}}.sql` · schema: `workloads/{{benchmark_name}}/schema.sql`
- Explore the test database with `backends/{{engine_name}}/host.sh sql`.
- Format and validation rules: `pipeline/prompts/shared/docs/test_suite.md` (the write-related files and keys do not apply).

{{change_request}}
