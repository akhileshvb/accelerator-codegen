#!/usr/bin/env bash
# Inter-query concurrency on DuckDB: R single-threaded readers + ONE writer applying the TPC-H refresh
# functions (official 0.1% update sets: RF1 inserts 1500 x SF orders with their lineitems, RF2 deletes
# 1500 x SF orders with theirs), swept over the interval between refreshes.  Each RF commit is followed
# by point writes on the tables RF never touches (supplier / part / partsupp; what Q2, Q11, Q16 read).
#
#   bash eval/compare/run_rf_interval_sweep.sh [out_dir]
#
# Per interval T (seconds between refresh slots) and query: run_compare.py concurrent on fresh copies of
# the database, base (DuckDB SQL readers, plain SQL writes) then cpp (accelerator readers with auxiliary
# state, writes maintained through it), the same parameter sets, writer stream, readers and run length.
# DuckDB runs with threads=1 (ACCEL_COMPARE_DUCKDB_THREADS=1): every reader -- DuckDB or accelerator --
# runs single-threaded on its own thread, and the writer's commits / maintenance scans are single-threaded
# in both systems.
#
# Env (defaults): SF=20, DB=../accelerator-codegen-duckdb/tpch_sf${SF}.duckdb,
#   RF_DIR=/flash1/akhilvb/tpch/refresh_sf${SF} (scripts/gen_tpch_refresh_sets.sh), WORK=/flash1/akhilvb/tpch/work,
#   INTERVALS="60 30 15 10" (seconds between refresh slots; from the SF20 cycle calibration every accelerator keeps up), READERS=8, QUERIES="1 ... 22", SYSTEMS=cpp,
#   REPEATS=3 (each a fresh parameter draw), SETS=8 (parameter sets per query and draw),
#   MIN_SECONDS=60 and MIN_SLOTS=4: each run lasts max(MIN_SECONDS, MIN_SLOTS * T) after a 5 s warm-up.
# Results: <out>/T<interval>s/conc_q<N>_<base|cpp>_r<k>.json (reader QPS / latency, commit latency, slots and
# rows touched per second, maintenance actions) and <out>/rf_interval_summary.csv (summarize_rf_interval_sweep.py).
#
# MODE=multi: per interval and system ONE process with every query's accelerator live (a database with many
# accelerators): the readers rotate over them and each writer commit maintains all of them, so the store protocol
# matters -- SYSTEMS="cpp verus_cpp verus" compares the C++ accelerators, the Verus kits on the C++ store (one lock
# per accelerator) and the Verus kits on the one verified world store (one lock for all).  The REPEATS draws run in
# the same process after one build, which happens with BUILD_THREADS DuckDB threads (default: all cores; outside
# the measured runs).  Results: <out>/T<interval>s/rep<k>/multi_<system>.json, summary in <out>/T<interval>s.log.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
SF="${SF:-20}"
DB="${DB:-${ROOT}/../accelerator-codegen-duckdb/tpch_sf${SF}.duckdb}"
RF_DIR="${RF_DIR:-/flash1/akhilvb/tpch/refresh_sf${SF}}"
WORK="${WORK:-/flash1/akhilvb/tpch/work}"
INTERVALS="${INTERVALS:-60 30 15 10}"
READERS="${READERS:-8}"
QUERIES="${QUERIES:-$(seq -s ' ' 1 22)}"
MODE="${MODE:-concurrent}"
SYSTEMS="${SYSTEMS:-cpp}"
BUILD_THREADS="${BUILD_THREADS:-$(nproc)}"
DUCKDB_MEM="${DUCKDB_MEM:-16GB}"
REPEATS="${REPEATS:-3}"
SETS="${SETS:-8}"
MIN_SECONDS="${MIN_SECONDS:-60}"
MIN_SLOTS="${MIN_SLOTS:-4}"
[[ "$MODE" == concurrent || "$MODE" == multi ]] || { echo "MODE must be concurrent or multi" >&2; exit 1; }
tag=""; [[ "$MODE" == multi ]] && tag="multi_"
OUT="${1:-${ROOT}/eval/compare/results/rf_interval_${tag}sf${SF}_$(date +%Y%m%d_%H%M%S)}"
SEED="${SEED:-$(od -An -N4 -tu4 /dev/urandom | tr -d ' ')}"  # one parameter-draw base for every interval

[[ -f "$DB" ]] || { echo "no database $DB" >&2; exit 1; }
[[ -f "$RF_DIR/delete.1" ]] || { echo "no TPC-H update sets in $RF_DIR (scripts/gen_tpch_refresh_sets.sh $SF)" >&2; exit 1; }
[[ -x build-verus-duckdb/aux_compare ]] || { echo "build aux_compare first: cmake --build build-verus-duckdb --target aux_compare" >&2; exit 1; }
nsets=$(ls "$RF_DIR"/delete.* | wc -l)
mkdir -p "$OUT"
export ACCEL_COMPARE_DUCKDB_THREADS=1
[[ "$MODE" == multi ]] && export ACCEL_COMPARE_DUCKDB_BUILD_THREADS="$BUILD_THREADS"
echo "mode=$MODE out=$OUT sf=$SF intervals=[$INTERVALS] readers=$READERS systems=$SYSTEMS repeats=$REPEATS sets=$SETS seed=$SEED rf_sets=$nsets start=$(date)" \
  | tee "$OUT/sweep.log"

for T in $INTERVALS; do
  secs=$(python3 -c "print(max(${MIN_SECONDS}, ${MIN_SLOTS} * ${T}))")
  slots=$(python3 -c "import math; print(math.ceil((${secs} + 5) / ${T}) + 1)")
  [[ "$MODE" == multi ]] && slots=$(( slots * REPEATS ))  # multi: the repetitions share one write stream
  if (( slots > 2 * nsets )); then
    echo "T=${T}s needs ~${slots} refresh commits per run but $RF_DIR has $nsets update sets (2 commits each); skipping" | tee -a "$OUT/sweep.log"
    continue
  fi
  wps=$(python3 -c "print(1.0 / ${T})")
  echo "=== T=${T}s (wps=${wps}, ${secs}s per run) start $(date +%T)" | tee -a "$OUT/sweep.log"
  extra=()
  [[ "$MODE" == multi ]] && extra=(--duckdb-threads 1 --duckdb-memory "$DUCKDB_MEM")
  python3 eval/compare/run_compare.py "$MODE" --db "$DB" --sf "$SF" --work "$WORK" \
    --rf-dir "$RF_DIR" --dim-after-rf --systems "$SYSTEMS" --queries "$QUERIES" \
    --readers "$READERS" --seconds "$secs" --wps "$wps" --sets "$SETS" --repeats "$REPEATS" --seed "$SEED" \
    "${extra[@]}" --out "$OUT/T${T}s" 2>&1 | tee "$OUT/T${T}s.log"
  echo "=== T=${T}s exit ${PIPESTATUS[0]} $(date +%T)" | tee -a "$OUT/sweep.log"
done
if [[ "$MODE" == concurrent ]]; then
  python3 eval/compare/summarize_rf_interval_sweep.py "$OUT" | tee -a "$OUT/sweep.log"
fi
