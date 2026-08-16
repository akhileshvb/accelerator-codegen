# Registration checklist

1. Implement `AuxQueryModule` subclass in `src/generated/{benchmark}/q{{query_number}}/aux/module.{hpp,cpp}`.
2. Export a factory (e.g. `MakeTpchQ{{query_number}}Module`) from `module.hpp`.
3. Register the factory in `src/generated/catalog/query_catalog.cpp` (append — do not split per query).
4. For Postgres: append SQL-callable dispatch in `src/generated/postgres/kernel_runner.cpp`.
5. CMake: `finalize.cpp` is picked up via `accel_register_tpch_aux_finalize_queries()`; test/bench/calibrate via `accel_register_tpch_aux_pipeline_targets()`. Manual `accel_add_aux_state_query(...)` in `CMakeLists.txt` is only needed when those globs are insufficient.
6. **DuckDB SQL table function (required, not automatic):** append `case {{query_number}}` in
   `src/stateful/duckdb/duckdb_aux_table_functions.cpp` (`RegisterWorkloadAuxFunctions`) that calls
   `RegisterAuxFunction(..., "{{aux_function_name}}", {LogicalType...})` with argument types matching
   the catalog / workload SQL (e.g. Q5: `VARCHAR, DATE`). Catalog entry + in-process
   `DuckdbRuntime` binding are **not** enough — calibrate / multi-conn / cross-accel / SQL benches
   call `SELECT * FROM {{aux_function_name}}(...)`. Missing this case fails with
   `Table Function with name {{aux_function_name}} does not exist`. Do **not** edit
   `src/runtime/duckdb/register.cpp` (it only invokes `RegisterDuckdbAuxTableFunctions`).
7. **Cross-accel / multi-connection:** append this query to `tests/test_support/cross_accel_helpers.cpp`:
   - `TpchAuxSql` / `TpchReferenceSql` (or SEC equivalents)
   - `InvalidatingWriterSqlForQuery` — a small `UPDATE` on a **watched** table/column (Q16 has no lineitem; use `part` / `partsupp` / `supplier`). Missing `case {{query_number}}` fails `aux_cross_accel` with `unsupported … invalidating writer SQL`.
8. **Bench arg variants:** append `case {{query_number}}` to `SampleTpchArgs` / `FormatTpchBaseSql` / `FormatTpchKernelSql` in `benchmarks/tpch/single-threaded/tpch_arg_variants.cpp`. The sweep sets `ACCEL_AUX_WRITE_FREQUENCIES` and samples args; a missing case fails `aux_bench_sweep` immediately (`SampleTpchArgs: unsupported query_id`).

Module contract:

- `Id()`, `Dependencies()` — accelerator identity and watched tables
- `CreateTxnBuffer()`, `Build()`, `InterpretMutation()`, `AccumulateMutation()`, `StageMaintenance()`
- `CheckValidity()`, `Execute()`, `ExecuteFallback()`

Concurrency: read `concurrency_contract.md` — irrelevant mutations NoOp, bounded read path, per-accelerator epochs.
