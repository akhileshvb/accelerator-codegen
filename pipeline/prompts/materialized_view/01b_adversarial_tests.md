# Stage 1b: Adversarial tests (matview)

Propose adversarial cases for **Q{{query_number}}** matview correctness and refresh behavior.

## Focus

- Parameter bindings at range edges / empty results
- Mutations on each dependent table (insert/update/delete) then **REFRESH** then re-query
- Stale-MV risk if refresh is skipped (document; gates always refresh)
- Concurrently refresh requirements (unique index) if selected

Write notes into the analysis artifact or `adversarial_tests.md` under query artifacts. Do not implement yet.

{{agent_boundary}}

{{constraints}}
