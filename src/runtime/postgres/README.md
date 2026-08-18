# PostgreSQL auxiliary-state prototype

This directory is the isolated PostgreSQL backend boundary. The default
`accelerator_codegen` target does not include PostgreSQL headers. Enable
`ACCELERATOR_CODEGEN_BUILD_POSTGRES=ON` with a PostgreSQL server development
installation to build the optional extension bridge.

The C++ core receives engine-neutral `MutationBatch` and `VisibilityToken`
values plus typed in-process state images. `PostgresStateStore` is the
serialization boundary for the LWLock-protected shared-memory image
directory: PostgreSQL stores byte images, while query modules see typed state
after the adapter deserializes them.

Base-table scans use the shared `NativeScanSpec` contract
(`runtime/native_scan.hpp`): `ScanNativeBaseTable` opens the relation via the
table AM (`table_beginscan` / `table_scan_getnextslot`), applies residual
filters in-process, and streams projected columns as DuckDB `DataChunk`s for
the same `KernelInstance` path DuckDB uses. The optional C extension still
uses SPI only for statement-trigger transition-table decoding into
`MutationBatch` values for `PostgresMutationCapture`.

## Calibrating the shared policy

An engine-specific calibration harness should seed
`core::CalibrateAuxCostModel` from the query's existing JSON configuration and
provide callbacks for the real PostgreSQL build, auxiliary SRF, fallback SQL,
delta transaction, rebuild, and invalidation operations. The resulting file
can be selected independently for each SQL-callable module with:

```sh
export ACCELERATOR_CODEGEN_POSTGRES_Q10_COST_MODEL=/path/q10-postgres.json
export ACCELERATOR_CODEGEN_POSTGRES_Q24_COST_MODEL=/path/q24-postgres.json
```

The extension reads these variables when each runtime is first constructed.
Each runtime also falls back to `config/q<query>_aux_cost_model.json` (TPC-H) or
`config/sec_edgar_q<query>_aux_cost_model.json` (SEC) when the env var is unset.
Search order:

1. per-query env var (for example `ACCELERATOR_CODEGEN_POSTGRES_Q6_COST_MODEL`)
2. `$ACCELERATOR_CODEGEN_CONFIG_DIR/<file>.json`
3. `config/<file>.json` relative to the server working directory
4. `/tmp/accel-codegen-src/config/<file>.json` (docker build tree)

Run `bash scripts/calibrate_postgres_aux_cost_models.sh` to refresh the JSON
coefficients before a performance sweep.
