#!/usr/bin/env bash
# Guarded EC2 SF3000 run: load, build the 22 accelerators, then with maintenance tracing on either exactly one RF1 and one
# RF2 (RF_SMOKE=1, the default: per-accelerator results and peak RSS, exit) or the whole QphH run (RF_SMOKE=0). A watchdog
# follows the run and, on a stalled commit, a rebuild fallback or low memory, captures evidence and kills it
# (scripts/perf/watchdog_refresh.sh).
#   scripts/perf/sf3000_rf_smoke.sh <out_dir> [extra run_compare.py tpch args]
# EC2 defaults (override by environment): SF=3000 CORES=0-447 DUCKDB_MEM=1500GB DB=/data/tpch_sf3000.duckdb
#   RF_DIR=/data/refresh_sf3000 WORK=/data/work SEED=1 TREE=new (this checkout)
# Local test: SF=20 CORES=0-39 DB=... RF_DIR=... WORK=... DUCKDB_MEM=16GB SKIP_SYSFS=1 NUMA=0 scripts/perf/sf3000_rf_smoke.sh out
# Watchdog knobs: WD_COMMIT_LIMIT_S (1200) WD_MEM_LIMIT_GB (1000) WD_INTERVAL_S (120); trace: ACCEL_AUX_TRACE_HEARTBEAT_*_S.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ $# -ge 1 ] || { sed -n 2,9p "$0"; exit 2; }
OUT=$1; shift
SF=${SF:-3000}; CORES=${CORES:-0-447}; SEED=${SEED:-1}; TREE=${TREE:-new}
export DB=${DB:-/data/tpch_sf3000.duckdb} RF_DIR=${RF_DIR:-/data/refresh_sf3000} WORK=${WORK:-/data/work}
mkdir -p "$OUT" "$WORK"

# THP: keep huge pages (the accelerators' reads are memory-bound) but never stall a fault on compaction, and keep
# khugepaged from collapsing (it queued on the process mmap_lock in the stalled runs); old values recorded
THP=/sys/kernel/mm/transparent_hugepage
if [ "${SKIP_SYSFS:-0}" != 1 ]; then
  for f in enabled defrag khugepaged/defrag khugepaged/max_ptes_none; do echo "$f=$(cat $THP/$f)"; done | tee "$OUT/thp_before.txt"
  echo always | sudo tee $THP/enabled > /dev/null
  echo defer | sudo tee $THP/defrag > /dev/null
  echo 0 | sudo tee $THP/khugepaged/defrag > /dev/null
  echo 0 | sudo tee $THP/khugepaged/max_ptes_none > /dev/null
  # a TB heap can need millions of mappings; past vm.max_map_count every allocation that maps memory fails
  echo "vm.max_map_count=$(cat /proc/sys/vm/max_map_count)" >> "$OUT/thp_before.txt"
  [ "$(cat /proc/sys/vm/max_map_count)" -ge 16777216 ] || sudo sysctl -q -w vm.max_map_count=16777216
  echo "now: $(for f in enabled defrag khugepaged/defrag khugepaged/max_ptes_none; do printf '%s=%s ' $f "$(cat $THP/$f)"; done)"
fi

# Allocator (ALLOC=glibc for the system one): jemalloc, so DuckDB's idle threads' malloc_trim (a walk of every glibc
# arena, madvising its free pages) has nothing to walk, with few arenas so freed memory is reused across threads (never
# purging, or one arena per thread, grew memory past twice the glibc peak at SF20)
if [ "${ALLOC:-jemalloc}" = jemalloc ]; then
  export LD_PRELOAD=${JEMALLOC:-/usr/lib/x86_64-linux-gnu/libjemalloc.so.2}
  export MALLOC_CONF=${MALLOC_CONF:-narenas:64,background_thread:true,dirty_decay_ms:10000,muzzy_decay_ms:0}
  [ -f "$LD_PRELOAD" ] || { echo "no $LD_PRELOAD (apt install libjemalloc2, or ALLOC=glibc)" >&2; exit 2; }
fi
echo "allocator: ${ALLOC:-jemalloc} ${MALLOC_CONF:-}" | tee "$OUT/allocator.txt"

export ACCEL_AUX_TRACE_MAINT=1
export ACCEL_COMPARE_PIDFILE=$OUT/aux_compare.pid
rm -f "$ACCEL_COMPARE_PIDFILE"
"$HERE/watchdog_refresh.sh" "$ACCEL_COMPARE_PIDFILE" "$OUT/tpch.log" "$OUT" > "$OUT/watchdog.out" 2>&1 &
WATCHDOG=$!

NUMACTL=""; [ "${NUMA:-1}" = 1 ] && NUMACTL="numactl --interleave=all"
PERF_FORCE=1 $NUMACTL "$HERE/run_qphh.sh" "$TREE" "$SF" "$CORES" "$OUT" --no-base --seed "$SEED" \
  --duckdb-memory "${DUCKDB_MEM:-1500GB}" --timeout "${TIMEOUT:-172800}" --no-check $([ "${RF_SMOKE:-1}" = 1 ] && echo --rf-smoke) "$@"
status=$?

kill "$WATCHDOG" 2> /dev/null; wait "$WATCHDOG" 2> /dev/null
echo "exit status $status; watchdog log $OUT/watchdog.log, trace lines: grep MAINT_TRACE $OUT/tpch.log"
compgen -G "$OUT/evidence_*" > /dev/null && echo "WATCHDOG FIRED: $(ls -d "$OUT"/evidence_*)"
exit $status
