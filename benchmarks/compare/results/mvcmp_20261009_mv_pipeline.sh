#!/bin/bash
# MV + IVM baseline phase of the accel-vs-MV comparison (2026-10-09).  Runs after the accelerator phase
# (results/mvcmp_20261009_accel120) has exited: the IMMVs' triggers on public would otherwise slow its writes.
#   1. build the parameter-agnostic views in public (mv_defs.py create)
#   2. rf-fixed, system mv, intervals 120 60 30 s (1.25k / 2.5k / 5k rows touched per s), 3 reps, seed 35489017
#   3. drop the views (their triggers would slow any later base / accel run on this database)
set -o pipefail
cd /home/akhilvb/accelerator-codegen/.worktrees/pgconc
source ~/accel_pg_env.sh
R=/home/akhilvb/accelerator-codegen/benchmarks/compare/results
OUT=$R/mvcmp_20261009_mv
mkdir -p $OUT
echo "waiting for the accelerator phase to exit $(date +%T)"
while pgrep -f 'run_backends.sh rf-fixed .*mvcmp_20261009_accel120' >/dev/null; do sleep 30; done
echo "accelerator phase done $(date +%T)"
tail -3 $R/mvcmp_20261009_accel120.console.log
echo "=== build views in public $(date +%T)"
$PYTHON benchmarks/compare/mv_defs.py create --schema public --jobs 8 2>&1 | tee $OUT/build.log || { echo BUILD FAILED; exit 1; }
echo "=== check views vs base (1 set per query) $(date +%T)"
$PYTHON benchmarks/compare/mv_defs.py check --schema public --sets 1 --seed 35489017 --out $OUT/check_draws 2>&1 | tee $OUT/check.log
echo "=== mv runs $(date +%T)"
SEED=35489017 PG_WORKERS=9 ONLY_PHASES=mv FIXED_INTERVALS="120 60 30" MV_SCHEMA=public MV_CHUNK=500 \
  bash benchmarks/compare/run_backends.sh rf-fixed $OUT 2>&1 | tee -i $OUT.console.log
echo "=== mv runs done $(date +%T); dropping the views"
$PYTHON benchmarks/compare/mv_defs.py drop --schema public 2>&1 | tee $OUT/drop.log
echo "=== pipeline done $(date +%T)"
