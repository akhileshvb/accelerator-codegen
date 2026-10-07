#!/usr/bin/env bash
# The C++ vs Verus accelerator experiments at SF20 (benchmarks/compare/run_compare.py).
#
#   bash benchmarks/compare/run_experiments.sh read      <out_root>   # read-only sweep
#   bash benchmarks/compare/run_experiments.sh writerate <out_root>   # write-rate sweep
#   bash benchmarks/compare/run_experiments.sh plots     <out_root>   # summaries + plots of both
#
# Every experiment repeats with fresh TPC-H parameter draws (REPEATS, default 3; the same
# draw for every system within a repetition, seeds in <dir>/seeds.json) and first records
# what it ran on (<dir>/meta/: git state + full diff, binary hashes, the installed Verus
# libraries, machine and load).  Results: one JSON per (query or rate, system, repetition).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
MODE=${1:?mode: read | writerate | plots}
OUT=${2:?out root, e.g. benchmarks/compare/results/final_$(date +%Y%m%d)}
REPEATS=${REPEATS:-3}
DB=${DB:-${ROOT}/../accelerator-codegen-duckdb/tpch_sf20.duckdb}
SF=${SF:-20}
BIN=${BIN:-$ROOT/build-verus-duckdb/aux_compare}
CAP=${CAP:-64G}                       # memory cap of each experiment process tree
RUN="python3 benchmarks/compare/run_compare.py"
mkdir -p "$OUT"

meta() {
  local d=$1/meta
  mkdir -p "$d"
  {
    echo "date: $(date -Is)"; echo "host: $(uname -n)"; echo "cmd: $0 $*"
    echo "repeats: $REPEATS  db: $DB  sf: $SF  bin: $BIN  cap: $CAP"
    echo "git HEAD: $(git rev-parse HEAD)"; echo "uncommitted files: $(git status --short | wc -l)"
    echo "aux_compare sha256: $(sha256sum "$BIN" | cut -c1-64)"
    echo "nproc: $(nproc)"; lscpu | grep -E 'Model name|Socket|Thread|NUMA node\(s\)'
    echo "load: $(cat /proc/loadavg)"; free -g
  } > "$d/meta.txt"
  git diff HEAD > "$d/code.diff"
  git status --short > "$d/git_status.txt"
  ls -la --time-style=full-iso build-verus/*.a build-verus/*.rlib > "$d/verus_libs.txt" 2>/dev/null
  sha256sum build-verus/*.a > "$d/verus_libs.sha256" 2>/dev/null
  cp benchmarks/compare/gen_tpch_params.py benchmarks/compare/run_compare.py \
     benchmarks/compare/aux_compare.cpp "$d/" 2>/dev/null
  ps -eo pid,user,%cpu,%mem,etime,comm --sort=-%cpu | head -25 > "$d/top_processes.txt"
}

capped() {  # run "$@" in a memory-capped scope, so an OOM kills only the experiment
  systemd-run --user --scope -p MemoryMax="$CAP" -p MemorySwapMax=0 "$@"
}

case $MODE in
read)
  # All 22 queries, C++ and Verus: one process per (query, system) builds the accelerator
  # once and answers every repetition's 8 parameter sets (5 timed runs each, and DuckDB's).
  D=$OUT/read; meta "$D" "$@"
  capped $RUN read --db "$DB" --sf "$SF" --bin "$BIN" --out "$D" --systems "cpp verus" \
    --repeats "$REPEATS" --sets 8 --reps 5 2>&1 | tee "$D/run.log"
  ;;
writerate)
  # All 22 accelerators in one process, 8 readers rotating over them, one writer whose
  # commit maintains all of them (TPC-H refresh + dimension point writes).  Systems:
  # DuckDB alone (base), C++ (one store and lock per accelerator), Verus kernels on
  # per-kit Verus stores (verus_kit: one lock per accelerator), Verus kernels on the one
  # global Verus store (verus: one lock for the whole database).  Each system builds once,
  # then runs every write rate x repetition (60 s measured after a 5 s warmup each).
  D=$OUT/writerate; meta "$D" "$@"
  capped $RUN multi --db "$DB" --sf "$SF" --bin "$BIN" --out "$D" --systems "cpp verus_kit verus" \
    --wps "${WPS:-2 5 10 20 40 -1}" --repeats "$REPEATS" --sets 8 --readers 8 --reader-mode rotate \
    --seconds 60 --warmup 5 --duckdb-memory "${DUCKDB_MEM:-8GB}" 2>&1 | tee "$D/run.log"
  ;;
plots)
  [ -d "$OUT/read" ] && $RUN summary "$OUT/read" | tee "$OUT/read/summary.txt"
  if [ -d "$OUT/writerate" ]; then
    for r in "$OUT"/writerate/wps* "$OUT"/writerate/uncapped; do
      [ -d "$r" ] && { echo "=== $(basename "$r")"; $RUN summary "$r"; }
    done | tee "$OUT/writerate/summary.txt"
    python3 benchmarks/compare/plot_wps_sweep.py "$OUT/writerate" | tee "$OUT/writerate/sweep_table.txt"
  fi
  ;;
*) echo "mode: read | writerate | plots"; exit 2 ;;
esac
