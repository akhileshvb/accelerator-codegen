# Aux unit-test contract (mandatory)

The module unit test is **shared, not written per query**: the module test of the engine's workload harnesses (made by `host_bootstrap`, `pipeline/prompts/shared/query_host_bootstrap.md`), built as `test_<bench>_q<N>_aux` and registered as ctest `<bench>_q<N>_aux_smoke_test` for every query that has registered its catalog entry. Do not add a per-query module test file.

It hosts your module alone on `ModuleRuntime` (the real aux runtime over a plain engine connection, no host or SQL entry point) over the small benchmark test database, and checks through the public module interface only:

1. build, then `Execute` for every argument variant of the harness's argument pool equals the reference SQL (the catalog entry's `base_sql`) **and** a second module built from scratch;
2. point writes: the catalog probe is written in SQL, its `MutationBatch` committed (`runtime.Commit`), and the answer re-checked after each write;
3. several probe rows in one SQL transaction delivered as one commit;
4. invalidation, then a rebuild.

So the test is only as strong as your **catalog entry** (the query's registration file, **registration** role):

- `base_sql`, `benchmark_arguments` and the `SharedProbe` from `build_probe` must follow the workload SQL (read `reference_sql_path`; do not invent filter polarity): `NOT LIKE`, `<>`, anti-joins and `NOT IN` keep their sense in the arguments you bind.
- `build_probe` must return probes that change the query result and whose `old_row`/`new_row` carry the module's `mutation_attnums` columns in order, with `update_sql` and `restore_sql` that match them. A probe that does not change the result fails the harness's invalidating-probe check and so the test.
- Edge cases of your module (NULLs, off-domain rows, empty inputs) belong in extra scenarios of the shared harness's module check, run for your query only, not in a private test file. Never weaken a check to make a kernel pass: if the module differs from a fresh build or from the SQL, the module is wrong.

Large-scale checks (magnitude databases, 1-vs-N thread comparison, decision-log / fallback counts) are the verification set of `scale_factor_contract.md`; they run the same shared differential and are not per-query tests.

## What `aux_correctness` checks

1. **Module test** - ctest `^<bench>_q<N>_aux_smoke_test$` (above), on the benchmark test-database fixture.
2. **Iteration-scale differential** - base-engine reference SQL vs aux execute for the catalog seed binding. The differential target is the engine's own (`host.sh info`, `host.sh run`; `query_host_bootstrap.md`).

Both must pass. Smoke-only success is not enough.
