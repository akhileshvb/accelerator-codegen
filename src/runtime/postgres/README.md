# PostgreSQL engine adapter

This directory is the PostgreSQL host boundary. The default
`accelerator_codegen` target does not include PostgreSQL server headers.
Enable `ACCELERATOR_CODEGEN_BUILD_POSTGRES=ON` with a PostgreSQL 17 server
SDK to build the optional `accelerator_codegen_pg` MODULE.

## Live path (per-engine, no DataChunk IR)

- **Base tables** stay in native Postgres heap. Aux `Build` / rebuild uses
  SPI typed InitialScans in `src/generated/postgres/runtime.cpp` and the
  table-AM helper `ScanPostgresTableForRebuild` in `scan_adapter.cpp`.
- **Base SRFs** (`accel_tpch_qN`) alias aux Execute via `kernel_executor.cpp`.
- **Mutations** go through AFTER ROW triggers →
  `accel_postgres_capture_mutation_trigger` → `postgres_bridge.cpp`
  (`BridgeState` observer + `PostgresAuxBackend::CaptureMutation`).
  Txn lifecycle is via extension xact hooks (`AccelXactCB`) and
  `postgres_xact_callbacks`.
- **Aux state** is published through `PostgresStateStore` (serialized
  boundary / bytea images as configured).

Docker (`accel_pg_test`) only changes how harnesses connect (TCP). The
in-process `BridgeState` singleton inside the MODULE remains the live
observer / toy / scan contract surface — it is not host-cluster scaffolding.

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
Each runtime also falls back to `config/q<query>_aux_cost_model.json` when the
env var is unset. Search order:

1. per-query env var (for example `ACCELERATOR_CODEGEN_POSTGRES_Q6_COST_MODEL`)
2. repo-relative `config/` JSON for that query
