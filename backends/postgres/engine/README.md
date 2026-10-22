# PostgreSQL engine adapter

This directory (`backends/postgres/engine/`) is the PostgreSQL host boundary; the rest of
the backend is in `state/`, `api/` (incl. `api/catalog`), `tests/`.
The module's SQL entry points are in `api/extension_api.cpp`, its engine glue in
`engine/extension_engine.cpp`. The default
`accelerator_codegen` target does not include PostgreSQL server headers.
Enable `ACCELERATOR_CODEGEN_BUILD_POSTGRES=ON` with a PostgreSQL 17 server
SDK to build the optional `accelerator_codegen_pg` MODULE.

## Live path (per-engine, no DataChunk IR)

- **Base tables** stay in native Postgres heap. Aux `Build` / rebuild uses
  SPI typed InitialScans in each query's `postgres/extension/scan.cpp`.
- **Base SRFs** (`accel_tpch_qN`) alias aux Execute via each query's
  `postgres/base/base.cpp`.
- **Backend vs. query code.** The backend (`backends/postgres/`: `api/runtime.{hpp,cpp}` registry + dispatch,
  `api/engine_helpers.hpp`, `api/engine_runtime.hpp`, `api/extension_api.cpp`,
  `engine/extension_engine.cpp`) names no query. Per-query code lives in
  `accelerators/<bench>/q<N>/postgres/{aux/catalog.cpp,
  extension/scan.cpp, extension/functions.cpp, base/base.cpp}` and registers
  through static registrars; `backends/postgres/backend.cmake` globs
  each query's `postgres/extension/*.cpp` and `postgres/base/*.cpp` into the MODULE. SQL functions are
  created by `SELECT accel_register_queries();` from the
  registered per-query `CREATE FUNCTION` text (see
  `backends/postgres/host/register.sql`).
- **Mutations** go through statement-level transition triggers →
  `AcceleratorCodegenCaptureTransition` (`engine/transition_capture.cpp`), which
  reads the transition tables into one columnar batch per statement and table
  (the union of the columns the active accelerators watch,
  `engine/accelerator_tables.*`). `engine/postgres_xact_callbacks.*` stages the
  batches per (sub)transaction and, at commit, takes a commit epoch of every
  accelerator that depends on a written table (`state/shared.*`: registry,
  epochs, gates) and ships one `CommitMessage` per accelerator to its shard
  worker. The writer-side tests are `tests/test_postgres_writer.cpp` over the
  `accelerator_codegen_pg_writer_test` MODULE.
- **Aux state** lives in the accelerators' shard workers (below), not in the
  backends.

Docker (`accel_pg_test`) only changes how harnesses connect (TCP). The
force-maintenance and last-decision UDFs live in the MODULE for differential
and calibration.

There is no portable DuckDB `DataChunk` scan IR on this host. Do not reintroduce
`kernel_runner`, `NativeScanSpec`→chunk conversion, or `postgres/base`
DataChunk kernels.

## Shard workers (`state/shard_*`)

Every active accelerator of a database has one background worker (a "shard")
that holds its one copy of the state on the substrate runtime
(`substrate/aux/runtime/`: `RuntimeHost`, `AcceleratorRuntime`, the default
`VersionedAuxStore`). The worker is a dynamic background worker started on
demand by the first backend that uses the accelerator (no
`shared_preload_libraries`); it exits on SIGTERM, on a Postgres error, or when
idle (`accel.shard_idle_exit_s`), and the next use starts it again.

- **Writes.** A transaction that wrote an accelerator's tables takes the next
  commit epoch of that accelerator at pre-commit (`state/shared.*`) and, after
  the commit, ships its captured batches to the worker's commit ring
  (`state/shard_segment.*`, fire and forget). The worker's commit thread
  offers them to the runtime, whose main-thread maintenance applies them in
  epoch order under a fresh snapshot (`EngineSlots::MaintenanceLookupEpoch`),
  as one change when several arrived together. A lost commit shows up as a gap
  and rebuilds; TRUNCATE ships an invalidation (rebuild).
- **Reads.** A statement that did not write the accelerator's tables takes its
  snapshot's epoch `k` (`shared::ReaderEpoch`) and asks the worker, through
  the request ring and a per-client response ring, for the answer at `k` once
  every commit through `k` is applied. A snapshot older than the version
  floor, a version collapsed by a combined apply, a worker that is not ready
  within `accel.read_wait_ms` (or `accel.read_timeout_ms`), or any worker
  problem makes the backend answer from SQL itself (the module's
  `ExecuteFallback`): never a wrong answer, only a slower one. Reads run on
  the worker's read threads (`accel.shard_read_threads`), which never call
  Postgres.
- **Builds** run on the worker's main thread (SPI / parallel scans through
  `api/base`), under a repeatable-read snapshot whose epoch is the build's;
  the first one waits out writers that were running when the worker started.
  The version floor is the epoch every open snapshot sees
  (`shared::HorizonEpoch`). `accel.threads` caps the threads of all of a
  database's workers (a shared token pool); `accel.read_threads`,
  `accel.build_threads` and `accel.maint_threads` cap what one read, build or
  maintenance pass of a worker may use of it (0: the budget, and 1 for
  maintenance; `ACCEL_AUX_{READ,BUILD,MAINT}_THREADS` in the server's
  environment override them).
- **Diagnostics.** `SELECT accel_shard_stats()` (counters summed over
  the running workers), `accel_stop_shard_workers()`,
  `accel_get_last_maintenance_decision()`,
  `accel_set_force_maintenance_action(text)`.
- **Known limits.** The workers run as the bootstrap superuser, so a backend
  without SELECT privilege on a dependency table can read aggregates the
  accelerator returns; a worker serves the state built under its own
  `search_path` (a session with another one answers from SQL); DDL on the
  tables can deadlock with a build in progress (stop the workers first).

## Calibrating the shared policy

An engine-specific calibration harness should seed
`core::CalibrateAuxCostModel` from the query's existing JSON configuration and
provide callbacks for the real PostgreSQL build, auxiliary SRF, fallback SQL,
delta transaction, rebuild, and invalidation operations. The resulting file
can be selected independently for each SQL-callable module with:

```sh
export ACCELERATOR_CODEGEN_POSTGRES_Q10_COST_MODEL=/path/q10-postgres.json
```

The shard worker reads these variables (from the server's environment) when it first needs the accelerator's cost model.
When the env var is unset, each runtime prefers a Postgres-calibrated file then
falls back to the shared DuckDB JSON. Search order:

1. per-query env var (for example `ACCELERATOR_CODEGEN_POSTGRES_Q6_COST_MODEL`)
2. `accelerators/tpch/q<query>/config/q<query>_aux_cost_model_postgres.json` (from
   `backends/postgres/bench/calibrate_postgres_tpch_aux_cost_model.cpp`)
3. `accelerators/tpch/q<query>/config/q<query>_aux_cost_model.json`
