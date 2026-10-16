# Shared setup of the perf scripts (sourced).  Resolves <tree>, <cores> and the equal core budget.
#   <tree>   "new" (accel-perf), "baseline" (accel-baseline) or a worktree path with build-release/aux_compare
#   <cores>  a taskset list, e.g. 32-39
# Knobs (environment): MAINT_THREADS (default: the core count), WORK (scratch copies, default /flash1/akhilvb/tpch/work).
PERF_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

perf_setup() {
  case "$1" in
    new) TREE="$PERF_ROOT" ;;
    baseline) TREE="$(dirname "$PERF_ROOT")/accel-baseline" ;;
    *) TREE="$(cd "$1" && pwd)" ;;
  esac
  CORES=$2
  [ -x "$TREE/build-release/aux_compare" ] || { echo "no $TREE/build-release/aux_compare (see docs/perf-runbook.md)" >&2; exit 1; }
  N=$(python3 -c 'import sys
n = 0
for part in sys.argv[1].split(","):
    lo, _, hi = part.partition("-")
    n += int(hi or lo) - int(lo) + 1
print(n)' "$CORES")
  # one budget everywhere: DuckDB's threads (baseline queries, builds) and the accelerators' read/build/maintenance pools
  export ACCEL_COMPARE_DUCKDB_THREADS=$N ACCEL_AUX_THREADS=$N ACCEL_AUX_READ_THREADS=$N ACCEL_AUX_BUILD_THREADS=$N
  export ACCEL_AUX_MAINT_THREADS=${MAINT_THREADS:-$N}
  export WORK=${WORK:-/flash1/akhilvb/tpch/work}
  RUN="taskset -c $CORES python3 $TREE/eval/compare/run_compare.py"
  grep -q -- "--baseline-sql-dir" "$TREE/eval/compare/run_compare.py" && SQLDIR_FLAG="--baseline-sql-dir $TREE/workloads/tpch" || SQLDIR_FLAG=""
}

# Refuses to start on a busy machine (1-minute load >= MAX_LOAD, default 4) unless PERF_FORCE=1.
perf_require_quiet() {
  local load
  load=$(cut -d' ' -f1 /proc/loadavg)
  if [ "${PERF_FORCE:-0}" != 1 ] && awk -v l="$load" -v m="${MAX_LOAD:-4}" 'BEGIN{exit !(l >= m)}'; then
    echo "machine is busy (1-min load $load >= ${MAX_LOAD:-4}); timings would be noise. Override with --force (run_all_sf20.sh) or PERF_FORCE=1." >&2
    exit 1
  fi
}
