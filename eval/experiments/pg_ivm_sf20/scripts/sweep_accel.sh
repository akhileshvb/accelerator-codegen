#!/usr/bin/env bash
# Paper run: burton, 2026-10-19/20. Writes $R/mvcmp: accel.console.log, accel_settings.txt, postgres/rf-fixed (data/ here).
# Phase 2: accelerator side of the accel-vs-views write-rate sweep (no views present). 8 readers x 10 threads,
# 80 maintenance threads; rates 0 / 50 / 200 / 1000 / 5000 rows/s + uncapped (300 s windows), 10 rows/s (900 s window).
set -uo pipefail
REPO=${REPO:-$(git rev-parse --show-toplevel)}
cd "$REPO"
R=${R:?set R to the work root holding mv/ and mvcmp/ (paper run: pg_sf20r)}
export RF_DIR=${RF_DIR:?set RF_DIR to the dbgen refresh sets (rf1/rf2 update files) for SF20}
O=$R/mvcmp; mkdir -p $O
export PATH=$REPO/.venv-pg-test/bin:$PATH PGPASSWORD=postgres PG_PORT=5460 PG_DB=accel_tpch_sf20
export PYTHON=python3 BACKENDS=postgres SF=20 READERS=8 PG_WORKERS=9 REPEATS=3 SEED=20261018 BATCH_ORDERS=500
export RF_EXTRA="--max-commits 15000" QUERY_TIMEOUT=1800 FIXED_WARMUP=30 MIN_READS=3 MAX_WINDOW=900 ONLY_PHASES=accel
PSQL="psql -h 127.0.0.1 -p 5460 -U postgres -d accel_tpch_sf20 -Atq"
echo "=== $(date) IVM triggers present: $($PSQL -c "select count(*) from pg_trigger where tgname like 'IVM_trigger%'") (must be 0)"
$PSQL -c "ALTER SYSTEM SET accel.read_threads = 10" -c "ALTER SYSTEM SET accel.maint_threads = 80" -c "ALTER SYSTEM SET accel.build_threads = 80" \
      -c "ALTER SYSTEM SET accel.threads = 80" -c "ALTER SYSTEM SET accel.shard_read_threads = 64" -c "SELECT pg_reload_conf()" >/dev/null
$PSQL -c "LOAD 'accelerator_codegen_pg'" -c "select string_agg(name||'='||setting, ' ' order by name) from pg_settings where name like 'accel.%threads' or name in ('max_parallel_workers','max_worker_processes')" | tee $O/accel_settings.txt
# rows per slot 2,686 (500 orders + point writes): 50 / 200 / 1000 / 5000 rows/s
FIXED_WINDOW=300 FIXED_INTERVALS="0 53.7 13.43 2.686 0.537 uncapped" bash eval/compare/run_backends.sh rf-fixed $O 2>&1 | tee -ia $O/accel.console.log
FIXED_WINDOW=900 FIXED_INTERVALS="268.6" FIRST_RESTART=never bash eval/compare/run_backends.sh rf-fixed $O 2>&1 | tee -ia $O/accel.console.log
echo "=== $(date) ACCEL SWEEP DONE"
