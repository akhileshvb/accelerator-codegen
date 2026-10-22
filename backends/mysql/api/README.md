# MySQL API manifest

A server plugin (`accelerator_codegen_mysql`) with UDF entry points and an audit plugin.
Roles and semantics: `pipeline/prompts/engine_backend/api_contract.md`.

## Contract roles

| role | headers | symbols |
|---|---|---|
| registration | `api/runtime.hpp`, `api/udf_shim.hpp`, `api/catalog/query_catalog.hpp` | `MysqlUdfRegistrar`, `RegisterMysqlUdf`, `ACCEL_MYSQL_QUERY_UDF`, `MysqlQueryCatalogRegistrar`, `RegisterMysqlQuery` |
| runtime | `api/mysql_host.hpp`, `api/runtime.hpp` | `MysqlHost`, `Runtime`, `TsvSink` |
| scan | `api/runtime.hpp` | `ScanTypedTuples`, `ScanTypedTuplesPartitioned`, `SnapshotScanSession`, `SplitTable`, `TypedInt`, `TypedScaledDecimal`, `KeyListPredicate` |
| sql_fallback | `api/runtime.hpp` | `AnswerFromMysqlSql`, `WithLiterals` |
| cost_model | `api/runtime.hpp` | `ResolveCostModelPath` |
| maintenance | `api/mysql_host.hpp` | `ForceMaintenanceAction`, `InvalidateAll`, `NoteRead`, `TotalStats` |
| introspection | | `accel_get_execute_counts`, `accel_get_last_maintenance_action`, `accel_get_last_maintenance_decision`, `accel_set_force_maintenance_action`, `accel_invalidate_all`, `accel_get_build_stats` |
| thread_knobs | `api/mysql_host.hpp` | `ThreadSettings`, `SetThreads` |
| value_semantics | `api/mysql_decimal.hpp`, `api/mysql_collation.hpp` | `Div`, `Avg`, `ToText`, `FoldAscii`, `EqualsCi`, `CompareCi` |

The introspection functions are defined in `api/mysql_plugin.cpp`. The sysvars are
`accel_threads`, `accel_read_threads` and `accel_build_threads` (defined in
`engine/mysql_bridge.cpp`; there is no `accel_maint_threads` yet).
