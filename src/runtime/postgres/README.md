# PostgreSQL engine adapter

This directory is the PostgreSQL host boundary. The default
`accelerator_codegen` target does not include PostgreSQL server headers.
Enable `ACCELERATOR_CODEGEN_BUILD_POSTGRES=ON` with a PostgreSQL 17 server
SDK to build the optional `accelerator_codegen_pg` MODULE.

## Live path (per-engine, no DataChunk IR)

- **Base tables** stay in native Postgres heap. Aux `Build` / rebuild uses
  SPI typed InitialScans in `src/generated/postgres/runtime.cpp`.
- **Base SRFs** (`accel_tpch_qN`) alias aux Execute via `kernel_executor.cpp`.
- **Mutations** go through statement-level transition triggers →
  `AcceleratorCodegenCaptureTransition` (`transition_capture.cpp`) →
  registered `PostgresAuxBackend` coordinators.
  Txn lifecycle is via `postgres_xact_callbacks` (`RegisterXactCallback`).
- **Aux state** is the per-query in-process `PostgresStateStore`
  (`stateful/postgres/`), not a serialized file-backed adapter.

Docker (`accel_pg_test`) only changes how harnesses connect (TCP). Observer
helpers (`accel_postgres_get_observer_mutation_count`, force-maintenance
UDFs) live in the MODULE for differential and calibration.

There is no portable DuckDB `DataChunk` scan IR on this host. Do not reintroduce
`kernel_runner`, `NativeScanSpec`→chunk conversion, or `postgres/base`
DataChunk kernels.

## Calibrating the shared policy

An engine-specific calibration harness should seed
`core::CalibrateAuxCostModel` from the query's existing JSON configuration and
provide callbacks for the real PostgreSQL build, auxiliary SRF, fallback SQL,
delta transaction, rebuild, and invalidation operations. The resulting file
can be selected independently for each SQL-callable module with:

```sh
export ACCELERATOR_CODEGEN_POSTGRES_Q10_COST_MODEL=/path/q10-postgres.json
```

The extension reads these variables when each runtime is first constructed.
When the env var is unset, each runtime prefers a Postgres-calibrated file then
falls back to the shared DuckDB JSON. Search order:

1. per-query env var (for example `ACCELERATOR_CODEGEN_POSTGRES_Q6_COST_MODEL`)
2. `config/q<query>_aux_cost_model_postgres.json` (from
   `scripts/calibrate_postgres_tpch_aux_cost_models.sh`)
3. `config/q<query>_aux_cost_model.json`
