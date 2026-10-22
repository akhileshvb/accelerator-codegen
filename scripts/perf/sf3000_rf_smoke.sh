#!/usr/bin/env bash
# RF smoke for the EC2 SF3000 run: load, build the 22 accelerators, run exactly one RF1 and one RF2 with maintenance tracing
# on, print per-accelerator results and peak RSS, exit. A watchdog follows the run and, on a stalled commit or low memory,
# captures evidence and kills it (scripts/perf/watchdog_refresh.sh).
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

# THP: madvise only, no direct compaction by khugepaged (it contended on the process mmap_lock); old values recorded
THP=/sys/kernel/mm/transparent_hugepage
if [ "${SKIP_SYSFS:-0}" != 1 ]; then
  { echo "enabled=$(cat $THP/enabled)"; echo "khugepaged_defrag=$(cat $THP/khugepaged/defrag)"; } | tee "$OUT/thp_before.txt"
  echo madvise | sudo tee $THP/enabled > /dev/null
  echo 0 | sudo tee $THP/khugepaged/defrag > /dev/null
  echo "now: enabled=$(cat $THP/enabled) khugepaged_defrag=$(cat $THP/khugepaged/defrag) (restore from $OUT/thp_before.txt)"
fi

export ACCEL_AUX_TRACE_MAINT=1
export ACCEL_COMPARE_PIDFILE=$OUT/aux_compare.pid
rm -f "$ACCEL_COMPARE_PIDFILE"
"$HERE/watchdog_refresh.sh" "$ACCEL_COMPARE_PIDFILE" "$OUT/tpch.log" "$OUT" > "$OUT/watchdog.out" 2>&1 &
WATCHDOG=$!

NUMACTL=""; [ "${NUMA:-1}" = 1 ] && NUMACTL="numactl --interleave=all"
PERF_FORCE=1 $NUMACTL "$HERE/run_qphh.sh" "$TREE" "$SF" "$CORES" "$OUT" --no-base --seed "$SEED" \
  --duckdb-memory "${DUCKDB_MEM:-1500GB}" --timeout "${TIMEOUT:-172800}" --no-check --rf-smoke "$@"
status=$?

kill "$WATCHDOG" 2> /dev/null; wait "$WATCHDOG" 2> /dev/null
echo "smoke exit status $status; watchdog log $OUT/watchdog.log, trace lines: grep MAINT_TRACE $OUT/tpch.log"
compgen -G "$OUT/evidence_*" > /dev/null && echo "WATCHDOG FIRED: $(ls -d "$OUT"/evidence_*)"
exit $status
