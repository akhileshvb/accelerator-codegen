# PostgreSQL engine adapter

This directory is the PostgreSQL host boundary. The default
`accelerator_codegen` target does not include PostgreSQL server headers.
Enable `ACCELERATOR_CODEGEN_BUILD_POSTGRES=ON` with a PostgreSQL 17 server
SDK to build the optional `accelerator_codegen_pg` MODULE.

## Live path (per-engine, no DataChunk IR)

- **Base tables** stay in native Postgres heap. Aux `Build` / rebuild uses
  SPI typed InitialScans in each query's `postgres/extension/scan.cpp`.
- **Base SRFs** (`accel_tpch_qN`) alias aux Execute via each query's
  `postgres/base/base.cpp`.
- **Backend vs. query code.** The backend (`src/generated/postgres/`:
  `runtime.{hpp,cpp}` registry + dispatch, `engine_helpers.hpp`,
  `engine_runtime.hpp`) names no query. Per-query code lives in
  `accelerators/<bench>/q<N>/postgres/{aux/catalog.cpp,
  extension/scan.cpp, extension/functions.cpp, base/base.cpp}` and registers
  through static registrars; `backends/postgres/engine/PostgresAdapter.cmake` globs
  `extension/*.cpp` and `base/*.cpp` into the MODULE. SQL functions are
  created by `SELECT accel_postgres_register_queries();` from the
  registered per-query `CREATE FUNCTION` text (see
  `scripts/postgres_register_tpch_all.sql`).
- **Mutations** go through statement-level transition triggers →
  `AcceleratorCodegenCaptureTransition` (`transition_capture.cpp`) →
  registered `PostgresAuxBackend` coordinators.
  Txn lifecycle is via `postgres_xact_callbacks` (`RegisterXactCallback`).
- **Aux state** is the per-query in-process `PostgresStateStore`
  (`stateful/postgres/`), not a serialized file-backed adapter.

Docker (`accel_pg_test`) only changes how harnesses connect (TCP). Observer
helpers (`accel_postgres_get_observer_mutation_count`, force-maintenance
UDFs) live in the MODULE for differential and calibration.

The engine_backend layer-gate UDFs (`pg_accel_{txn,mutations,scan,runtime,vis,host}_*`,
toy modules over a test bridge) are not in this MODULE. They live in
`backends/postgres/tests/extension_test_*.cpp` and build into a separate
test-only MODULE, `accelerator_codegen_pg_test` (the backend sources plus those
files, no per-query code), which the layer harnesses `LOAD`.
`scripts/build_accel_postgres_extension.sh` installs it when a requested target
depends on it (or `ACCEL_PG_TEST_MODULE=1`).

There is no portable DuckDB `DataChunk` scan IR on this host. Do not reintroduce
`kernel_runner`, `NativeScanSpec`→chunk conversion, or `postgres/base`
DataChunk kernels.

## Shared accelerator state (`accel.shared_state`)

By default every backend holds a private copy of each accelerator's state,
built on its first query, so memory grows with connections x state size and
every new connection pays a build. With `accel.shared_state = on` a
per-database background worker holds the one copy and all backends use it
(`stateful/postgres/aux_server.{hpp,cpp}`, design notes at the top of the
header). Per-backend mode is unchanged and stays the default.

- **Enable.** `SET accel.shared_state = on` (any scope), or server wide:
  `ALTER SYSTEM SET accel.shared_state = on; SELECT pg_reload_conf();`
  (sandbox: `pgsbx.sh shared <name> on`). Set it before the first accelerator
  call or write of a session; switching a live session back to `off` is
  supported but its private copy then resyncs.
- **No `shared_preload_libraries` needed.** The worker is a dynamic background
  worker started on demand by the first backend that needs it, and the shared
  memory comes from the DSM registry (PostgreSQL 17). It loads the same
  module (`accelerator_codegen_pg`) the backends use, so the module must be
  findable from `$libdir` (as for the SQL functions) and `max_worker_processes`
  needs one free slot per active database. Preloading is optional.
- **Reads.** The backend sends the SQL function's oid, arguments, search_path
  and its statement snapshot to the worker (a `shm_mq` pair per backend); the
  worker runs the same function under that snapshot and streams tuples back.
  It answers only if its state provably equals the database as that snapshot
  sees it (the `aux_sync` counters and cover/see checks, unchanged); else
  (or if the worker is busy/unreachable past `accel.shared_timeout_ms`, or
  fails) the backend answers from SQL as today. A transaction's own writes to
  a dependency table always take the SQL path.
- **Writes.** Captured transition rows are staged per transaction (with
  savepoint frames) and shipped at COMMIT, after the counters are published,
  under a transaction-scoped commit gate so shipped commits are applied one at
  a time in commit order. The worker applies a commit incrementally only when
  the accelerator has exactly one captured statement in it (a later
  statement's lookups would otherwise see rows the earlier one did not);
  anything else, an unshipped commit, or an error leaves the shared counters
  ahead of the worker's marker, and it rebuilds on the next query. Nothing
  uncommitted ever reaches the worker; a backend that dies mid-transaction
  releases its in-flight counters through the existing abort callback.
- **Worker lifecycle.** `SELECT accel_postgres_stop_shared_worker()` (or
  `pg_terminate_backend` on the `accel aux server` backend) stops it; the next
  use restarts it and rebuilds. A worker holds a connection to its database:
  `DROP DATABASE` needs `WITH (FORCE)` and `CREATE DATABASE ... TEMPLATE` needs
  the worker stopped first (`accel.shared_idle_exit_s` makes it exit when
  idle). `SELECT accel_postgres_shared_stats()` shows its counters.
  Functions are installed like the other helpers
  (`CREATE FUNCTION accel_postgres_shared_stats() RETURNS text AS
  'accelerator_codegen_pg' LANGUAGE C`).
- **Known limits.** The worker runs as the bootstrap superuser, so a backend
  without SELECT privilege on a dependency table can read aggregates the
  accelerator returns (enable it only where every user may read those
  tables); one worker per database serialises queries and builds (a build
  blocks that database's other queries up to `accel.shared_timeout_ms`);
  forced maintenance actions and the decision log are bridged through shared
  memory for the differential harness only.

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
2. `accelerators/tpch/q<query>/config/q<query>_aux_cost_model_postgres.json` (from
   `scripts/calibrate_postgres_tpch_aux_cost_models.sh`)
3. `accelerators/tpch/q<query>/config/q<query>_aux_cost_model.json`
