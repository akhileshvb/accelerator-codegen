# PostgreSQL API manifest

A server extension (`accelerator_codegen_pg`) with shard-worker processes. Roles and
semantics: `pipeline/prompts/engine_backend/api_contract.md`.

## Contract roles

| role | headers | symbols |
|---|---|---|
| registration | `api/runtime.hpp`, `api/accelerators.hpp`, `api/catalog/query_catalog.hpp` | `PostgresQueryExecutorRegistrar`, `PostgresQuerySqlRegistrar`, `ExecutePostgresQuery`, `PostgresAcceleratorRegistrar`, `PostgresQueryCatalogRegistrar`, `RegisterPostgresQuery` |
| runtime | `api/engine_runtime.hpp`, `api/accelerators.hpp`, `api/pg_sink.hpp`, `api/base/runtime_internal.hpp` | `Runtime`, `RuntimeRegistrar`, `PostgresAccelerator`, `PgSink` |
| scan | `api/engine_helpers.hpp`, `api/base/pg_base.hpp` | `ScanPartitioned`, `ScanTypedTuples`, `BuildBatch`, `DecimalColumn`, `TypedText`, `ScanTablePartitioned`, `KernelRegistrar` |
| sql_fallback | `api/sql_fallback.hpp` | `AnswerFromPostgresSql` |
| cost_model | `api/engine_helpers.hpp` | `ResolveCostModelPath` |
| maintenance | `api/engine_runtime.hpp` | `CostModel`, `TuneBuild` |
| introspection | | `accel_get_execute_counts`, `accel_get_last_maintenance_action`, `accel_get_last_maintenance_decision`, `accel_set_force_maintenance_action`, `accel_shard_stats`, `accel_stop_shard_workers` |
| thread_knobs | | |
| value_semantics | `api/pg_numeric.hpp`, `api/pg_numeric_div.hpp` | `AvgOfScaledSum`, `DivScale`, `DivRound`, `ToText` |

The introspection functions are defined in `api/extension_api.cpp`. The thread knobs are the
`accel.threads`, `accel.read_threads`, `accel.build_threads` and `accel.maint_threads` GUCs,
defined in `state/shard_gucs.cpp` (outside `api/`), so the role lists no header.
