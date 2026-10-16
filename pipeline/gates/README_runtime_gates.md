# Backend slot gates

Gates for the runtime-skeleton contracts (`substrate/aux/runtime/README.md`),
run in the backend stage's validation (`post_validation_gates` in
`config/engine_backend.yaml`) against any generated backend. They are
backend-generic: one script (`run_slot_gates.sh`) maps the gate name to a
scenario and an engine **adapter** runs it.

| Gate | Scenario | Pass criteria |
|---|---|---|
| `slot_epoch_stress` | A writer commits continuously; readers in every reader mode read the accelerator and the base SQL in one snapshot, twice with pauses | Every read equals the reference at its snapshot (no torn or stale read); no reader error; after the load all answers exact |
| `slot_hits_under_writes` | Continuous commits, readers read back to back, per isolation level the engine has (Postgres: READ COMMITTED and REPEATABLE READ) | Exact answers; accelerator hit rate >= the minimum (0.8 by default); reads under per-statement snapshots are checked only while no commit is in flight |
| `slot_read_after_commit` | Writer commits on one connection and another connection reads right after, while other readers keep the accelerators busy | Every such read includes the commit |
| `slot_txn_edges` | Rollback, savepoint rollback / release (where supported), a table-wide TRUNCATE-and-restore | Exact answers throughout; no rebuild after aborts; the accelerator serves again after TRUNCATE |
| `slot_restart` | Accelerator workers / processes restarted under load | Exact answers across the restart; hits resume. Skipped (not applicable) for in-process engines |
| `slot_tsan` | The four scenarios above under ThreadSanitizer (`run_aux_tsan.sh slots`) | No report with a frame in `substrate/`, `backends/` or `accelerators/` |
| `slot_differential` | The 22-query differential and the RF differential | Every query matches the base SQL |

Capture completeness is part of every scenario: all writes come from plain
connections, never from the connection that registered the accelerator, and
every answer is compared with the base SQL.

## Harness and adapters

- **Generic scenarios**: `eval/harness/slot_gates.{hpp,cpp}`, written once
  against `slot_gates::Engine` (connect, accelerated read cases with their
  reference SQL, reader modes, a write source, accelerator counters, truncate,
  savepoints, restart).
- **Adapter per engine**: `slot_adapters/<engine>.sh` defines
  `slot_adapter_run <scenario> <seconds>` and exits 0 (pass), 1 (fail) or
  77 (not applicable). `ACCEL_SLOT_GATE_SECONDS` sets the load duration.

| Engine | Adapter | State |
|---|---|---|
| DuckDB | `slot_adapters/duckdb.sh` -> `eval/tests/test_slot_gates_duckdb.cpp` (the DuckDB `Engine` driver; workload = TPC-H Q1 and Q6 over lineitem, writes from `tpch_fuzz_writes.hpp`) and `test_tpch_shared_differential` / `test_shared_tpch_rf_differential` | Runnable on the host |
| Postgres | `slot_adapters/postgres.sh` -> scenarios of `eval/tests/test_postgres_shared_state.cpp` and `scripts/run_postgres_tpch_diff_all22.sh`, run in `accel_pg_test` | Needs the shard-worker port (branch `skel/pg-worker`); lacks a READ COMMITTED hit gate, a cross-connection read-after-commit and an RF differential (listed in the adapter) |
| MySQL | `slot_adapters/mysql.sh` -> `test_mysql_aux_multi_connection_stress`, `_build_race`, `_session_isolation` and `scripts/run_mysql_tpch_diff_all22.sh`, run in `accel_mysql_test` | Needs the MySQL port (branch `skel/mysql`); lacks a hit rate, a read right after a slow commit (the stale-read case) and an RF differential (listed in the adapter) |

A backend gets full coverage by implementing the `Engine` driver for its
engine (as `test_slot_gates_duckdb.cpp` does) and pointing its adapter at it.
A missing driver or missing test source makes the adapter fail closed.

Known limit of the DuckDB workload: the host maps a captured table to one
`TableId`, so only queries that number a table identically can be registered
together (Q1 and Q6 both use lineitem = 1). `ACCEL_SLOT_GATE_QUERIES` selects
the queries; combining queries with different ids fails the gates by design.

## ThreadSanitizer (`run_aux_tsan.sh`)

`run_aux_tsan.sh <query> [engine]` (gate `aux_tsan`, aux-state audit and required
whenever a module has parallel read/build/maintenance code) and
`run_aux_tsan.sh slots` (gate `slot_tsan`) build a cached `ACCEL_SANITIZE=thread`
tree (`build-tsan`, or `ACCEL_TSAN_BUILD_DIR`; the engine stays uninstrumented) and
run the query's smoke test and shared differential, or the slot scenarios, with
`ACCEL_AUX_READ/BUILD/MAINT_THREADS=8` (`ACCEL_TSAN_THREADS`) and
`ACCEL_AUX_TEST_GRAIN=1`, a test-only cap on every slice grain so even one-row
commits split across threads. It fails on a mismatch or a report whose stack
touches our code; reports confined to the engine are listed in the logs and
ignored (`tsan.supp` is for engine races that cross our frames, never for our
code). Logs: `<artifacts>/gates/aux_tsan_logs/`. Only the in-process DuckDB host
is run; other engines skip.

## Not yet gates

Planned from the design note, covered today only inside layer harnesses or
by observation: rebuild under continuous writes without a quiet window
(`runtime` layer; `builds` counter in the epoch/hits reports), reads during a
long apply (latency bound), shifted-key and value-inflated SF1 (run by hand today: the verification set of `pipeline/prompts/shared/scale_factor_contract.md`), 1-vs-N phase-cap comparison, peak threads under `taskset`
(`ThreadBudget::PeakThreads`), apply time and bytes copied per changed row at
SF1 vs SF20.

`run_thread_budget_lint.sh` (static, `hardware_concurrency` and fixed thread
counts in `accelerators/` and `backends/`) is pattern based: it misses counts
computed through helpers or differently named variables; mark a reviewed line
`// thread-budget-ok: <why>`. Report-only by default; `--strict` exits
non-zero. Known offenders at the time of writing: q3/q9/q17 DuckDB modules cap
`WorkerCount()` at 8; Postgres/MySQL q9/q13/q16/q20/q2/q11 use
`hardware_concurrency`; `backends/duckdb/api/duckdb_runtime.cpp`.
