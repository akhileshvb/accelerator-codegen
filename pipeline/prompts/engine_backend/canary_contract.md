# The canary suite (pipeline-owned) and its binding

The pipeline ships a suite of accelerator-facing checks under `pipeline/canary/`. It drives the
backend the way accelerators do (SQL sessions, registered accelerators, the typed build scan, the
runtime's counters) and judges every answer against the engine's own SQL. **You do not write or
change the checks.** You bind the suite to your backend: one file, `backends/{engine}/tests/test_{engine}_canary.cpp`
(target `test_{engine}_canary`, registered by the `tests/test_{engine}_*.cpp` glob of `backend.cmake`),
from the template `pipeline/templates/engine_backend/canary_binding.cpp.tpl`. Layer `api` ships it; the
gates are `engine_canary` (plain build), `engine_canary_asan` (our code under ASan + UBSan) and
`engine_canary_tsan` (the concurrency checks under TSan). The sanitizer tiers build a separate build dir
with `-DACCEL_SANITIZE=address|thread` (root `CMakeLists.txt`) and run on embedded engines.

## The binding: `canary::Engine` and `canary::Session` (`pipeline/canary/canary_engine.hpp`)

| member | the backend provides |
|---|---|
| `Session::Query(sql)` | one client connection (own transaction state and snapshot); every statement, rows as NULL / BIGINT / VARCHAR cells; an engine error throws `std::runtime_error` |
| `Engine::Connect()` | a new session on the engine's database (thread-safe) |
| `Engine::Register(spec)` | registers the accelerator `spec` through the backend's own registration (the SQL entry-point registrar of `api_contract.md`): its SQL name, its `fallback_sql` = `spec.sql`, its capture tables. `spec.make_module(scan)` builds the pipeline's module around the `ScanFn` you pass it. Module table id `i` is the engine table `spec.tables[i]`: you map ids to tables **by name**; two accelerators use the same ids for different tables |
| `Engine::ReadSql(name)` | the SQL a client sends to read the accelerator |
| `Engine::ReadCounters(name)` | the runtime's own statistics for it: reads served from state, reads answered from SQL, completed builds, commits the module could not maintain. Never computed or assumed |
| `Engine::Invalidate(name)` | its state is rebuilt before the next hit |
| `Engine::ForceFallback(b)` | every read answered from SQL (the substrate's forced fallback) |
| `Engine::TrapFallback(b)` | fallback SQL execution becomes an error (`ACCEL_FALLBACK_TRAP`, `host_contract.md` "Hit contract") |
| `Engine::Settle()` | storage reaches its steady state (checkpoint, compaction), where compressed and dictionary-encoded columns exist |
| `ScanBatch` | your `ScanPartitioned` row view, forwarded **as it is**: `Int`, `TextView`, `IsNull` of the engine's batch; no copy, no cache, no fixing up |

Rules of the binding: it is a pass-through. It never hides an error, retries, copies text the engine
view would have invalidated, builds state itself, or reads the answer anywhere but through
`ReadSql`. A check that fails is a defect of the backend, found by a test you did not write.

## What the suite checks (each failure names its owning layer)

| check | owner | defect class it exists for |
|---|---|---|
| `text_views_short_and_long` | scan | text views of short (inlined) and long strings, all alive together until the sink returns (`native_scan_contract.md`), dangling after the chunk is released |
| `dictionary_and_nulls` | scan | dictionary-encoded vectors and NULL cells mis-scanned, serial and partitioned |
| `scan_growth_and_delete` | scan | the partitioned scan duplicating or losing rows after the table grew or lost whole row groups; serial and partitioned equal SQL as multisets |
| `hit_after_first_read` | api | reads never build the state; a hit that still runs fallback SQL (the fallback is trapped) |
| `dml_incremental_hits` | capture | INSERT / UPDATE / DELETE forcing a rebuild, or the hit rate dropping under writes (real counters) |
| `bulk_insert_threads` | capture | capture racing under concurrent multi-threaded `INSERT ... SELECT` |
| `multi_table_by_name` | runtime | module table ids not mapped to engine tables by name (colliding ids, one multi-table accelerator) |
| `fallback_snapshot_and_order` | api | the fallback at the wrong snapshot, without the transaction's own writes, or without the query's ORDER BY |
| `decline_snapshot` | runtime | the answer after a module declines taken at the wrong snapshot |
| `commit_build_concurrency` | runtime | commit/build deadlock, or a wrong answer after racing rebuilds (bounded in time) |

Each check runs in its own process on a fresh database, bounded in time (a hang fails that check).
`test_{engine}_canary --list`, `--check <id>`, `--owner <layer>` select checks; one JSON line per check.

## Server engines

The accelerators run inside the server. The canary's modules (`pipeline/canary/canary_mirror.hpp`,
substrate-only) are compiled into the plugin's test-only variant and registered by table name
through the SQL entry-point registrar; the binding's sessions are the engine's client connections,
`Register` checks they are installed, and the counters come from the introspection hooks
(`api_contract.md` §3). The sanitizer tiers are skipped for them.
