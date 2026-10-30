#!/usr/bin/env bash
# Figure 8 (A) and Figure 1 (bottom, stateful bar): full-machine write-rate sweep of JetStream stateful (cpp) vs
# DuckDB, 40 vs 40. Writes $OUT/T{60,30,15,10,5}s/conc_q*_{base,cpp}_r0.json, then rf_interval_summary.csv
# (data/writerate/ holds the paper run).
# Cores 0-39, DuckDB threads = accelerator read/build/maint threads = 40; 8 readers, one writer applying the
# official TPC-H RF1/RF2 update sets (1500 x SF = 30,000 orders each at SF20) + dimension point writes after each
# RF; write interval T = 60 30 15 10 5 s, run = max(60, 3T) s, 1 repetition, 8 parameter sets.
# Paper run: tree accelerator-codegen at 29ed49f7 (build-release/aux_compare).
# Required: DB (tpch_sf20.duckdb), RF_DIR (refresh sets), OUT. Optional: REPO.
set -uo pipefail
REPO=${REPO:-$(git rev-parse --show-toplevel)}
DB=${DB:?TPC-H SF20 DuckDB database}
RF_DIR=${RF_DIR:?TPC-H SF20 refresh sets}
OUT=${OUT:?output directory}
cd "$REPO"
. scripts/perf/common.sh
perf_setup new 0-39
perf_record_settings "$OUT"
SEED=$(od -An -N4 -tu4 /dev/urandom | tr -d ' ')
until awk '{exit !($1 < 2)}' /proc/loadavg; do sleep 30; done
echo "mode=concurrent intervals=[60 30 15 10 5] systems=cpp seed=$SEED commit=$(git rev-parse --short HEAD) start=$(date)" | tee "$OUT/sweep.log"
for T in 60 30 15 10 5; do
  secs=$(python3 -c "print(max(60, 3 * $T))"); wps=$(python3 -c "print(1.0 / $T)")
  $RUN concurrent --db "$DB" --sf 20 --work "$WORK" --bin "$TREE/build-release/aux_compare" $SQLDIR_FLAG \
    --rf-dir "$RF_DIR" --dim-after-rf --systems cpp --queries "$(seq -s ' ' 1 22)" \
    --readers 8 --seconds "$secs" --wps "$wps" --sets 8 --repeats 1 --seed "$SEED" \
    --out "$OUT/T${T}s" 2>&1 | tee "$OUT/T${T}s.log"
done
python3 eval/compare/summarize_rf_interval_sweep.py "$OUT" | tee -a "$OUT/sweep.log"
