# Postgres SF20 read-only sweep: plain Postgres vs the stateful (aux) accelerators

Mirrors the DuckDB read sweep (`docs/perf-runbook.md`): per TPC-H query, build the accelerator, then `--repeats 5` fresh
random parameter draws x `--sets 8` parameter sets; speedup is the geomean over a draw's sets, reported as median
[min-max] over the draws, with build time, answer mismatches and shard-worker fallbacks.

## Prerequisites

- Container `accel_pg_sf20` (postgres:17, 127.0.0.1:5460), database `accel_tpch_sf20` (SF20, 19 indexes, analyzed; data
  dir `/flash1/akhilvb/pg_sf20/data`), loaded with `SKIP_EXTENSION=1` (extension not installed).
- Python 3 with `psycopg2`; Docker; a DuckDB fork checkout (`$DUCKDB`, default `../duckdb-dbcapture`).

## Steps

1. `scripts/perf/pg_sf20/build_ext.sh` from the final commit's checkout (read-only mount, build dir `$BUILD`; set
   `DUCKDB_BUILD` to a prebuilt DuckDB build to skip its ~30 min build), then `install_ext.sh` (copies the libraries into the
   server container and runs `register.sql`: accelerator functions + capture triggers). Compiling must not overlap timing.
2. `set_gucs.sh`: `max_parallel_workers_per_gather`, `max_parallel_workers`, `accel.threads|read_threads|build_threads|
   maint_threads|shard_read_threads` = 8, restart, print the settings. The container's command line has
   `-c max_parallel_workers=48`, which overrides ALTER SYSTEM: the server-side value stays 48 and `sweep.py` sets 8 per
   session (recorded in `<out>/session_gucs.txt`). Thread GUCs are server settings; client env vars do nothing.
3. Quiet window (other timing jobs finished, 1-minute load < 2, no `cc1plus`/`ninja`):
   `OUT=/flash1/akhilvb/perf/<run>/pg_read/out GATES="<marker files>" scripts/perf/pg_sf20/run_sweep.sh`
   It pins the container to `CORES` (32-39), holds `$LOCK` while timing (other agents pause on it), and restores the
   pinning on exit. Smoke test first: `... run_sweep.sh --queries "6 14" --repeats 1 --sets 2`.

## Method

- Plain Postgres: one untimed warm-up on a query's first case, then one timed run per case. Accelerator: stop the shard
  workers (cold build), first call builds (build = first call minus the median read), then per case one untimed result
  call (compared with the plain answer, rows canonicalized) and 5 timed calls (median). The OS cache is warm for both.
- Baseline text: `workloads/tpch/qN.sql` with the draw's parameters; accelerator: `SELECT * FROM accel_tpch_qN_aux(args)`.
- Fallbacks = delta of `accel_postgres_shard_stats()` `fallback` over the query; must be 0 with `requests == served`.

## Results and runtime

`<out>/cases.csv` (per case), `qstats.jsonl` (build time, worker stats, load at start/end per query),
`mismatch_q*_r*_s*.json` (any mismatch, with both answers), `summary.md` (`summarize.py <out>`). Expect about 3-5 hours
(extrapolated from two smoke queries at ~3.7 s per plain run; Q9/Q18/Q21 and similar will dominate).
