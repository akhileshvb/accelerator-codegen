#!/bin/bash
# MV + IVM maximum write throughput (2026-10-09): one run, writer uncapped (back-to-back commits) for two refresh
# slots (RF1 + its 6 point writes, RF2 + its 6), 8 readers on; then drop the views.  The rate sweep was stopped
# after its first run: one MV RF1 commit takes ~13 min (pg_ivm), longer than any of the 120/60/30 s intervals.
set -o pipefail
cd /home/akhilvb/accelerator-codegen/.worktrees/pgconc/benchmarks/compare
source ~/accel_pg_env.sh
OUT=/home/akhilvb/accelerator-codegen/benchmarks/compare/results/mvcmp_20261009_mv/postgres/throughput
mkdir -p $OUT
echo "=== mv throughput run $(date +%T)"
$PYTHON run_sql_backend.py writerate --backend postgres --db $PG_DB --systems mv --schema public --chunk 500 \
  --rates -1 --seconds 7200 --warmup 0 --readers 8 --dim-after-rf --rf-dir $RF_DIR --max-write-commits 14 \
  --repeats 1 --only-rep 0 --sets 4 --pg-workers 9 --seed 35489017 --query-timeout 1800 --out $OUT 2>&1 | tee -i $OUT/run.log
echo "=== throughput run done $(date +%T); dropping the views"
$PYTHON mv_defs.py drop --schema public 2>&1 | tee $OUT/../../drop.log
echo "=== done $(date +%T)"
