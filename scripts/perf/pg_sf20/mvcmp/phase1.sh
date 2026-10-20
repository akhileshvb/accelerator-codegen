#!/usr/bin/env bash
# Phase 1 (no full view set present): writer-alone maintenance cost per design, Q21/Q22 refresh modes, accel write limit.
set -uo pipefail
cd /home/akhilvb/accelerator-codegen
M=/disk3/akhilvb_tpch/pg_sf20r/mv; RF=/disk3/akhilvb_tpch/refresh_sf20; SEED=20261018
export PATH=/home/akhilvb/accelerator-codegen/.venv-pg-test/bin:$PATH PGPASSWORD=postgres PG_PORT=5460 PG_DB=accel_tpch_sf20
PSQL="psql -h 127.0.0.1 -p 5460 -U postgres -d accel_tpch_sf20 -Atq"
W=(--backend postgres --db accel_tpch_sf20 --sf 20 --rates -1 --seconds 86400 --warmup 0 --readers 0 --dim-after-rf
   --rf-dir $RF --batch-orders 500 --query-timeout 1800 --repeats 1 --sets 1 --max-write-commits 28 --seed $SEED --pg-workers 9)
run_mv() {  # <queries> <out> [env...]
  local q=$1 D=$2; shift 2
  mkdir -p $D; env "$@" python3 -B eval/compare/mv_defs.py create --schema public --jobs 4 --queries "$q" 2>&1 | tail -2
  env "$@" python3 -B eval/compare/run_sql_backend.py writerate "${W[@]}" --queries "$q" --out $D --systems mv --schema public --chunk 500 \
    2>&1 | tee $D/run.log | grep -v -E 'NOTICE|shard_read' | grep -E 'qps|error|Error|STOPPED' | cut -c1-400
  env "$@" python3 -B eval/compare/mv_defs.py drop --schema public | tail -1
}
for q in 2 3 10 15; do for d in v1 v2; do
  echo "=== $(date) q$q $d"; run_mv "$q" $M/maint/q${q}_$d MV_DESIGN=$d
done; done
echo "=== $(date) q21 concurrent 80 workers"; run_mv 21 $M/maint/q21_concurrent_w80 MV_DESIGN=v1 MV_REFRESH=concurrent
echo "=== $(date) q21 concurrent 9 workers";  run_mv 21 $M/maint/q21_concurrent_w9 MV_DESIGN=v1 MV_REFRESH=concurrent MV_REFRESH_WORKERS=9
echo "=== $(date) q21 plain 80 workers";      run_mv 21 $M/maint/q21_plain_w80 MV_DESIGN=v1 MV_REFRESH=plain
echo "=== $(date) q22 concurrent";            run_mv 22 $M/maint/q22_concurrent MV_DESIGN=v1 MV_REFRESH=concurrent
echo "=== $(date) q22 plain";                 run_mv 22 $M/maint/q22_plain MV_DESIGN=v1 MV_REFRESH=plain
echo "=== $(date) accel writer-alone limit (IVM triggers left: $($PSQL -c "select count(*) from pg_trigger where tgname like 'IVM_trigger%'"))"
D=$M/maint/accel_limit; mkdir -p $D
python3 -B eval/compare/run_sql_backend.py writerate "${W[@]}" --queries "$(seq -s ' ' 1 22)" --out $D --systems accel \
  2>&1 | tee $D/run.log | grep -v -E 'NOTICE|shard_read' | grep -E 'qps|error|Error|STOPPED' | cut -c1-400
echo "=== $(date) PHASE1 DONE"
