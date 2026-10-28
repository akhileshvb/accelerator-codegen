# DuckDB API manifest

The embedded engine: the backend is compiled into `libaccelerator_codegen`; accelerators
register table functions on a connection. Roles and semantics: `pipeline/prompts/engine_backend/api_contract.md`.

## Contract roles

| role | headers | symbols |
|---|---|---|
| registration | `api/register.hpp`, `api/aux_query_registry.hpp`, `api/catalog/query_catalog.hpp`, `api/query_runtime.hpp`, `api/kernel.hpp`, `api/function_lifecycle.hpp`, `api/shared_query_engine.hpp`, `api/verus_host.hpp`, `api/declining_query.hpp` | `RegisterAcceleratorFunctions`, `BaseFunctionSetRegistrar`, `AuxQueryRegistrar`, `RegisterAuxQuery`, `SharedQueryRegistrar`, `RegisterSharedQuery`, `QueryPackage`, `CreateRuntimeBackedFunctionSet`, `KernelInstance`, `FunctionLifecycle`, `SharedQueryEngine`, `MakeVerusKitBinding`, `DecliningQuery`, `RegisterDecliningQuery`, `DecliningQueryRegistrar` |
| runtime | `api/duckdb_host.hpp`, `api/duckdb_runtime.hpp`, `api/duckdb_aux_backend.hpp`, `api/parallel/parallel_runtime.hpp`, `api/parallel/parallel_kernel.hpp`, `api/parallel/parallel_scale_profile.hpp` | `DuckdbHost`, `DuckdbRuntime`, `DuckDBAuxBackend`, `ParallelWorkerCount`, `ParallelKernelInstance`, `ParallelScanMetrics` |
| scan | `api/scan.hpp`, `api/build_scan.hpp`, `api/duckdb_scan_adapter.hpp`, `api/native_scan.hpp`, `api/native_scan_compat.hpp`, `api/chunk_source.hpp`, `api/dictionary_id_cache.hpp`, `api/parallel/aux_parallel_scan.hpp` | `ScanQuery`, `ScanQueryStreaming`, `BuildColumn`, `BindNativeScan`, `NativeScanSpec`, `ChunkSource`, `DictionaryIdCache` |
| sql_fallback | `api/sql_fallback.hpp` | `FallbackSql`, `AnswerFromFallbackSql`, `WithLiterals` |
| cost_model | `api/cost_model_path.hpp` | `SharedCostModelConfigPath` |
| maintenance | `api/duckdb_runtime.hpp` | `SetExpectedQueriesBeforeNextWrite`, `CalibrationWrites`, `DuckdbCalibrationEngine`, `InvalidateAll` |
| introspection | | |
| thread_knobs | | |
| value_semantics | `api/decimal128.hpp` | `HugeintToInt128`, `Decimal128Cell` |

`introspection` and `thread_knobs` are empty: DuckDB does not yet register the
`accel_*` hook functions of the contract, and its thread caps are the substrate's
`ACCEL_AUX_*_THREADS` environment only (no engine setting). Both are open items.
