#!/usr/bin/env bash
# Figure 1 (bottom): refresh stream at T=15 s (~10k rows/s), DuckDB base + JetStream stateless + verified, 40 vs 40.
# Writes $OUT/T15s/conc_q*_{base,stateless,verus}_r*.json (data/headline_T15/ holds the paper run; Q21 stateless
# there is the rerun after the LIMIT 100 fix). The stateful system comes from drive_writerate.sh (T15s).
# Same framing as drive_writerate.sh: cores 0-39, every thread knob 40, 8 readers, one writer on the official
# RF1/RF2 sets + dimension writes, 60 s runs, 8 parameter sets, 3 repetitions with fresh draws.
# Paper run: tree verus_compat at 29ed49f7, build-verus-duckdb linked against its re-verified Verus world.
# Required: TREE (tree with build-verus-duckdb/aux_compare), DB (tpch_sf20.duckdb), RF_DIR (refresh sets), OUT.
set -uo pipefail
TREE=${TREE:?tree with build-verus-duckdb/aux_compare}
DB=${DB:?TPC-H SF20 DuckDB database}
RF_DIR=${RF_DIR:?TPC-H SF20 refresh sets}
OUT=${OUT:?output directory}
BIN=${BIN:-$TREE/build-verus-duckdb/aux_compare}
WORK=${WORK:-$OUT/work}
CORES=0-39 N=40
export ACCEL_COMPARE_DUCKDB_THREADS=$N ACCEL_AUX_THREADS=$N ACCEL_AUX_READ_THREADS=$N ACCEL_AUX_BUILD_THREADS=$N ACCEL_AUX_MAINT_THREADS=$N
SEED=$(od -An -N4 -tu4 /dev/urandom | tr -d ' ')
mkdir -p "$OUT"
cd "$TREE"
{ echo "tree=$TREE commit=$(git rev-parse --short HEAD) bin=$BIN cores=$CORES budget=$N"
  env | grep -E '^ACCEL_(COMPARE_DUCKDB|AUX)_[A-Z_]*THREADS=' | sort; } > "$OUT/thread_settings.txt"
until awk '{exit !($1 < 2)}' /proc/loadavg; do sleep 30; done
echo "mode=concurrent T=15s systems=base,stateless,verus seed=$SEED start=$(date)" | tee "$OUT/sweep.log"
taskset -c $CORES python3 eval/compare/run_compare.py concurrent --db "$DB" --sf 20 --work "$WORK" --bin "$BIN" \
  --baseline-sql-dir "$TREE/workloads/tpch" --rf-dir "$RF_DIR" --dim-after-rf --systems "stateless verus" \
  --queries "$(seq -s ' ' 1 22)" --readers 8 --seconds 60 --wps 0.06666666666666667 --sets 8 --repeats "${REPEATS:-3}" \
  --seed "$SEED" --out "$OUT/T15s" 2>&1 | tee "$OUT/T15s.log"
rm -rf "$WORK"
