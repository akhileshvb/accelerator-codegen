#!/usr/bin/env bash
# One system's rf-fixed sweep (read-only + RF every 120/60/30 s = ~1,250/2,500/5,000 rows/s), 8 readers x 5 procs,
# server on socket 0 (40 threads), clients on socket 1.  Same driver, draws, write stream and chunk for both systems.
#   sweep.sh mv|accel FIRST_REP REPEATS
set -uo pipefail
source /spinning/akhilvb/pg40/scripts/env.sh
SYS=$1 FIRST_REP=${2:-0} REPEATS=${3:-1}
OUT=$W/results/sweep_$SYS
export BACKENDS=postgres ONLY_PHASES=$SYS FIXED_INTERVALS="0 120 60 30" FIRST_REP REPEATS READERS PG_WORKERS=$PGW \
       SEED RF_DIR=$RF PG_PORT PG_DB PYTHON=python3 MIN_READS=3 MAX_WINDOW=900 QUERY_TIMEOUT=1800 \
       MV_SCHEMA=public MV_CHUNK=500 RF_EXTRA="--chunk 500 --check-timeout 600" \
       FIXED_WINDOW=900 FIXED_WARMUP=30
export MV_DESIGN="2 3 10 15"     # chosen designs (results/phase1): v2 for Q2/3/10/15, v1 elsewhere
export MV_REFRESH_WORKERS=$PGW   # the writer's refreshes get a reader's per-session parallel budget
pin_server
echo "=== $(date) sweep $SYS reps $FIRST_REP..$((REPEATS-1)) -> $OUT"
$CLIENT bash $REPO/eval/compare/run_backends.sh rf-fixed $OUT
echo "=== $(date) SWEEP $SYS DONE"
