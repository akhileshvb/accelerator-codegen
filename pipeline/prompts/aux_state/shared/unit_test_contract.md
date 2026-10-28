# Aux unit-test contract

The module unit test is shared, not per query: the workload harness's module test (made by `host_bootstrap`, `query_host_bootstrap.md`), built as `test_<bench>_q<N>_aux`, ctest `<bench>_q<N>_aux_smoke_test`, for every query with a registered catalog entry. Add no per-query module test file.

It hosts your module alone on `ModuleRuntime` (the real aux runtime on a plain engine connection, no host or SQL entry point) over the small test database and checks, through the public module interface only:

1. After build, `Execute` for every argument variant of the harness's argument pool equals the reference SQL (catalog `base_sql`) and a second module built from scratch.
2. Point writes: the catalog probe is written in SQL, its `MutationBatch` committed (`runtime.Commit`), the answer re-checked after each write.
3. Several probe rows in one SQL transaction delivered as one commit.
4. Invalidation, then a rebuild.

The test is only as strong as your catalog entry (the query's registration file):

- `base_sql`, `benchmark_arguments` and the `SharedProbe` from `build_probe` follow the workload SQL (`reference_sql_path`); keep filter polarity: `NOT LIKE`, `<>`, anti-joins and `NOT IN` keep their sense in the bound arguments.
- A workload that draws its parameters at run time (`params_generator` in its benchmark pack) has no default binding: the catalog entry holds `$n` templates only (`base_sql`, `fallback_sql`, the aux SQL calling the table function with `$1, ...`, a parameterized `probe_select_sql`), `benchmark_arguments` is the signature alone (`core::AuxValue::Null(type)` per `$n`), the table function has no zero-argument form, and state, config and finalize code carry no parameter values. Bind a template with `BindArguments`; tests and benches run the sets of `harness::ArgumentSets(entry)` (in Python, `param_sets.draw_sets`), print the `[params] ... seed=` line and compare the aux result with the template bound to the same arguments, never with a stateless kernel.
- `build_probe` returns probes that change the query result, whose `old_row`/`new_row` carry the module's `mutation_attnums` columns in order, with matching `update_sql` and `restore_sql`. A probe that does not change the result fails the invalidating-probe check.
- Module edge cases (NULLs, off-domain rows, empty inputs) go in extra scenarios of the shared harness's module check for your query, not a private test. Never weaken a check to pass a kernel: a module that differs from a fresh build or from the SQL is wrong.

Large-scale checks (magnitude databases, 1-vs-N threads, decision-log and fallback counts) are the verification set of `scale_factor_contract.md`; they run the same shared differential.

## `aux_correctness`

1. Module test: ctest `^<bench>_q<N>_aux_smoke_test$` on the test-database fixture.
2. Iteration-scale differential: engine reference SQL vs aux execute for the catalog's binding (the drawn parameter sets when it has none), using the engine's own differential target (`host.sh info`, `host.sh run`; `query_host_bootstrap.md`).

Both must pass; smoke alone is not enough.
