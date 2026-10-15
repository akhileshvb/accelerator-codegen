# Runtime gates (specs)

Gates for the runtime-skeleton contracts (`substrate/aux/runtime/README.md`). Each is implemented as a test harness with its backend (`backends/{engine}/tests/`, driven from `eval/`). **None is wired into the orchestrator yet**; `run_thread_budget_lint.sh` is the only script that exists. `GATE_SCRIPTS` in `orchestrator/accel_pipeline/gates.py` gets an entry when a harness lands.

| Gate (planned name) | Scenario | Pass criteria | Harness |
|---|---|---|---|
| `runtime_no_stale_reads` | Writer commits; once the commit returns, another connection reads. Repeat under concurrent writers | Every read issued after a commit returned includes that commit (0 stale reads) | `test_{engine}_runtime_visibility` |
| `runtime_capture_completeness` | Writes (insert/update/delete) from a second connection to tracked tables, then compare accelerator vs base-table SQL | Identical results; 0 missed changes | `test_{engine}_runtime_capture` |
| `runtime_rebuild_under_writes` | Invalidate an accelerator, then write continuously with no quiet window | Accelerator reaches synced within the time bound while writes continue; results exact | `test_{engine}_runtime_rebuild` |
| `runtime_read_latency_during_apply` | Reads while a long apply (bulk change) runs | p99 read latency within the bound; reads never wait behind the apply | `test_{engine}_runtime_apply_latency` |
| `runtime_hits_under_writes` | Mixed read/write load. Postgres: READ COMMITTED and REPEATABLE READ | Aux hit rate >= the minimum; results exact | `test_{engine}_runtime_hits` |
| `runtime_thread_budget` | Run under `taskset` with a fixed budget; sample peak threads of the process | Peak threads <= budget (`threads` / `accel.threads` / `accel_threads` / `ACCEL_AUX_THREADS`) | `test_{engine}_runtime_threads` |
| `runtime_shifted_keys_sf1` | SF1 with every key + 2^33 over substrate and backend paths | Results equal the unshifted SF1 results | `test_{engine}_runtime_shifted_keys` |
| `runtime_maintenance_scaling` | Same write workload at SF1 and SF20 | Apply time per changed row, and bytes copied per commit / rows changed, stay flat (SF20/SF1 within the tolerance) | `test_{engine}_runtime_scaling` |
| `thread_budget_lint` | Static scan of `accelerators/` and `backends/` for `hardware_concurrency` and fixed thread counts | Report-only by default; `--strict` exits non-zero on any offender | `run_thread_budget_lint.sh` (exists) |

Numeric bounds (latency, hit rate, tolerance, time to synced) are set when the harness is implemented and recorded in its test.

`run_thread_budget_lint.sh` is pattern based: it flags `hardware_concurrency`, `min(WorkerCount(), <literal>)` and literal thread/worker counts >= 2. It misses counts computed through helpers or differently named variables; mark a reviewed line `// thread-budget-ok: <why>`. Known offenders today: q3/q9/q17 DuckDB modules cap `WorkerCount()` at 8; Postgres/MySQL q9/q13/q16/q20/q2/q11 use `hardware_concurrency`; `backends/duckdb/api/duckdb_runtime.cpp`.
