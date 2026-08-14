# Registration checklist

1. Implement `AuxQueryModule` subclass in `src/generated/{benchmark}/q{{query_number}}/aux/module.{hpp,cpp}`.
2. Export a factory (e.g. `MakeTpchQ{{query_number}}Module`) from `module.hpp`.
3. Register the factory in `src/generated/catalog/query_catalog.cpp` (append — do not split per query).
4. For Postgres: append SQL-callable dispatch in `src/generated/postgres/kernel_runner.cpp`.
5. CMake: `finalize.cpp` is picked up via `accel_register_tpch_aux_finalize_queries()`; test/bench/calibrate via `accel_register_tpch_aux_pipeline_targets()`. Manual `accel_add_aux_state_query(...)` in `CMakeLists.txt` is only needed when those globs are insufficient.
6. DuckDB table function `{{aux_function_name}}` is wired through the catalog + `src/runtime/duckdb/register.cpp` scaffold (agents do not edit register.cpp directly).

Module contract:

- `Id()`, `Dependencies()` — accelerator identity and watched tables
- `CreateTxnBuffer()`, `Build()`, `InterpretMutation()`, `AccumulateMutation()`, `StageMaintenance()`
- `CheckValidity()`, `Execute()`, `ExecuteFallback()`

Concurrency: read `concurrency_contract.md` — irrelevant mutations NoOp, bounded read path, per-accelerator epochs.
