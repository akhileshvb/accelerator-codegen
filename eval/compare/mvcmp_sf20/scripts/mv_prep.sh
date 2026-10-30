#!/usr/bin/env bash
# Materialized-view baseline prep on accel_pg_sf20r, after the stateless sweep: install pg_ivm, build the current (v1)
# views + the read-optimized v2 candidates, check them all against plain Postgres (8 draws + domain ends per query),
# then measure writer-alone maintenance of v1 vs v2 per changed query. Stops before any sweep.
set -uo pipefail
cd /home/akhilvb/accelerator-codegen
M=/disk3/akhilvb_tpch/pg_sf20r/mv; R=/disk3/akhilvb_tpch/pg_sf20r
export PATH=/home/akhilvb/accelerator-codegen/.venv-pg-test/bin:$PATH PGPASSWORD=postgres PG_PORT=5460 PG_DB=accel_tpch_sf20
PSQL="psql -h 127.0.0.1 -p 5460 -U postgres -d accel_tpch_sf20 -Atq"
SEED=20261018; V2Q="2 3 10 15"
until [ -e $R/STATELESS_FINAL ]; do sleep 60; done
echo "=== $(date) stateless done; pg_ivm"
docker exec -u root accel_pg_sf20r bash -lc 'test -f /usr/share/postgresql/17/extension/pg_ivm.control || (apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq postgresql-17-pg-ivm)' || exit 1
$PSQL -c "CREATE EXTENSION IF NOT EXISTS pg_ivm" -c "SELECT extversion FROM pg_extension WHERE extname='pg_ivm'"
$PSQL -c "SELECT accel_postgres_stop_shard_workers()" >/dev/null 2>&1
echo "=== $(date) build v1 (all 22) + v2 ($V2Q)"
MV_DESIGN=v1 python3 -B eval/compare/mv_defs.py create --schema public --jobs 8 2>&1 | tee $M/build_v1.log || exit 1
MV_DESIGN=v2 python3 -B eval/compare/mv_defs.py create --schema public --jobs 4 --queries "$V2Q" 2>&1 | tee $M/build_v2.log || exit 1
echo "=== $(date) check v1 (8 draws + edges, 9 workers like a reader)"
MV_DESIGN=v1 python3 -B eval/compare/mv_defs.py check --schema public --sets 8 --edges --seed $SEED --workers 9 --out $M/check_draws 2>&1 | tee $M/check_v1.log
echo "=== $(date) check v2"
MV_DESIGN=v2 python3 -B eval/compare/mv_defs.py check --schema public --sets 8 --edges --seed $SEED --workers 9 --queries "$V2Q" --out $M/check_draws 2>&1 | tee $M/check_v2.log
echo "=== $(date) drop all views for the maintenance A/B"
MV_DESIGN=v2 python3 -B eval/compare/mv_defs.py drop --schema public; MV_DESIGN=v1 python3 -B eval/compare/mv_defs.py drop --schema public
for q in $V2Q; do for d in v1 v2; do
  echo "=== $(date) maintenance q$q $d: build its views, writer alone (2 RF slots of 500 orders + point writes)"
  MV_DESIGN=$d python3 -B eval/compare/mv_defs.py create --schema public --jobs 4 --queries "$q" 2>&1 | tail -3
  D=$M/maint/q${q}_$d; mkdir -p $D
  MV_DESIGN=$d python3 -B eval/compare/run_sql_backend.py writerate --backend postgres --db accel_tpch_sf20 --sf 20 --out $D \
    --queries "$q" --rates -1 --seconds 86400 --warmup 0 --readers 0 --dim-after-rf --rf-dir $R/../refresh_sf20 \
    --batch-orders 500 --query-timeout 1800 --repeats 1 --sets 1 --max-write-commits 14 --systems mv --schema public \
    --chunk 500 --seed $SEED --pg-workers 9 2>&1 | tee $D/run.log | grep -v -E 'NOTICE|shard_read' | tail -4
  MV_DESIGN=$d python3 -B eval/compare/mv_defs.py drop --schema public
done; done
# Q21 / Q22 (plain views refreshed in each commit): CONCURRENTLY (readers never blocked) vs plain REFRESH
for q in 21 22; do for mode in concurrent plain; do
  echo "=== $(date) maintenance q$q refresh=$mode: build its views, writer alone"
  MV_DESIGN=v1 python3 -B eval/compare/mv_defs.py create --schema public --jobs 4 --queries "$q" 2>&1 | tail -3
  D=$M/maint/q${q}_$mode; mkdir -p $D
  MV_REFRESH=$mode MV_DESIGN=v1 python3 -B eval/compare/run_sql_backend.py writerate --backend postgres --db accel_tpch_sf20 --sf 20 --out $D \
    --queries "$q" --rates -1 --seconds 86400 --warmup 0 --readers 0 --dim-after-rf --rf-dir $R/../refresh_sf20 \
    --batch-orders 500 --query-timeout 1800 --repeats 1 --sets 1 --max-write-commits 14 --systems mv --schema public \
    --chunk 500 --seed $SEED --pg-workers 9 2>&1 | tee $D/run.log | grep -v -E 'NOTICE|shard_read' | tail -4
  MV_DESIGN=v1 python3 -B eval/compare/mv_defs.py drop --schema public
done; done
# The accelerator's write limit on this build (views all dropped above, so no pg_ivm triggers): writer alone, back
# to back, the sweep's slot shape (500 orders + point writes), 80 maintenance threads; the shard workers build first.
echo "=== $(date) accelerator writer-alone limit (views: $($PSQL -c "select count(*) from pg_trigger where tgname like 'IVM_trigger%'") IVM triggers left)"
D=$M/maint/accel_limit; mkdir -p $D
python3 -B eval/compare/run_sql_backend.py writerate --backend postgres --db accel_tpch_sf20 --sf 20 --out $D \
  --queries "$(seq -s ' ' 1 22)" --rates -1 --seconds 86400 --warmup 0 --readers 0 --dim-after-rf --rf-dir $R/../refresh_sf20 \
  --batch-orders 500 --query-timeout 1800 --repeats 1 --sets 1 --max-write-commits 28 --systems accel \
  --seed $SEED --pg-workers 9 2>&1 | tee $D/run.log | grep -v -E 'NOTICE|shard_read' | tail -6
echo "=== $(date) MV PREP DONE"
