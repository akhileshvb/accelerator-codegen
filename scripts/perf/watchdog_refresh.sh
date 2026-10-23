#!/usr/bin/env bash
# Watchdog for a long accelerator run (see sf3000_rf_smoke.sh): follows the aux_compare process, logs its memory and thread
# states, and on a stalled refresh or low available memory captures evidence and kills the run.
#   scripts/perf/watchdog_refresh.sh <pidfile> <trace_log> <out_dir>
#     pidfile    written by aux_compare (ACCEL_COMPARE_PIDFILE); the watchdog waits for it, exits when the process is gone
#     trace_log  where aux_compare's stderr goes (MAINT_TRACE lines, ACCEL_AUX_TRACE_MAINT=1): <out>/tpch.log
#     out_dir    watchdog.log (a sample every WD_INTERVAL_S) and evidence_<time>/ on a trigger
# Triggers: a traced span (commit / refresh / engine-statement / engine-commit / precommit) open longer than
# WD_COMMIT_LIMIT_S (default 1200), MemAvailable below WD_MEM_LIMIT_GB (default 1000), or an accelerator falling back to a
# rebuild (a STALE trace line; WD_STALE_KILL=0 to only log it).
# Knobs: WD_INTERVAL_S=120  WD_POLL_S=10 (trigger check period)  WD_PERF_S=60  WD_PERF_HZ=49  WD_KILL_GRACE_S=60  WD_KILL=1 (0: capture only)
# Evidence needs sudo for /proc/PID/task/*/stack and perf; perf record -g, not eu-stack or gdb (those stop the process).
set -uo pipefail
[ $# -eq 3 ] || { sed -n 2,13p "$0"; exit 2; }
PIDFILE=$1; TRACE=$2; OUT=$3
INTERVAL=${WD_INTERVAL_S:-120}; POLL=${WD_POLL_S:-10}; LIMIT=${WD_COMMIT_LIMIT_S:-1200}
MEM_LIMIT_KB=$(awk -v g="${WD_MEM_LIMIT_GB:-1000}" 'BEGIN{printf "%d", g * 1024 * 1024}')
PERF_S=${WD_PERF_S:-60}; GRACE=${WD_KILL_GRACE_S:-60}
mkdir -p "$OUT"; LOG=$OUT/watchdog.log
SUDO=""; [ "$(id -u)" -ne 0 ] && sudo -n true 2> /dev/null && SUDO="sudo -n" # else best effort as this user
say() { echo "$(date +%T) $*" | tee -a "$LOG" >&2; }

while [ ! -s "$PIDFILE" ]; do sleep 2; done
PID=$(tr -d '[:space:]' < "$PIDFILE")
say "allocator in pid $PID: $(grep -m1 -o 'libjemalloc[^ ]*' /proc/$PID/maps 2>/dev/null || echo glibc)"
say "watching pid $PID (interval ${INTERVAL}s, span limit ${LIMIT}s, MemAvailable limit $((MEM_LIMIT_KB / 1048576)) GB)"

meminfo_kb() { awk -v k="$1:" '$1 == k {print $2}' /proc/meminfo; }

sample() {
  local rss
  rss=$(awk '$1 == "VmRSS:" {print $2}' /proc/$PID/status 2>/dev/null)
  {
    echo "== $(date +%T) rss_kb=${rss:-?} mem_available_kb=$(meminfo_kb MemAvailable) anon_huge_kb=$(meminfo_kb AnonHugePages) threads=$(ls /proc/$PID/task 2>/dev/null | wc -l) maps=$(wc -l < /proc/$PID/maps 2>/dev/null)"
    echo "thp: $(grep -E '^thp_(fault_alloc|fault_fallback|collapse_alloc|collapse_alloc_failed|split_page|zero_page_alloc|deferred_split_page) ' /proc/vmstat | tr '\n' ' ')"
    echo "wchan: $(awk 1 /proc/$PID/task/*/wchan 2>/dev/null | sort | uniq -c | sort -rn | head -8 | awk '{printf "%s=%s ", $2, $1}')"
    echo "state: $(cat /proc/$PID/task/*/stat 2>/dev/null | sed 's/^.*) \(.\) .*/\1/' | sort | uniq -c | awk '{printf "%s=%s ", $2, $1}')"
  } >> "$LOG"
}

# The oldest traced span that began and has not ended: "<age_s> <span>".
oldest_open_span() {
  grep -a '^MAINT_TRACE ' "$TRACE" 2>/dev/null | awk -v now="$(date +%s.%N)" '
    { t = substr($2, 3) + 0; tok = $4 }
    $5 == "begin" { open[tok] = t }
    $5 == "end" { delete open[tok] }
    END { for (k in open) if (best == "" || open[k] < best_t) { best = k; best_t = open[k] }
          if (best != "") printf "%d %s\n", now - best_t, best }'
}

capture_evidence() {
  local why=$1 dir=$OUT/evidence_$(date +%Y%m%d_%H%M%S)
  mkdir -p "$dir"
  say "TRIGGER: $why; evidence in $dir"
  echo "$why" > "$dir/reason.txt"
  local perf_pid=""
  if command -v perf > /dev/null; then
    $SUDO perf record -F "${WD_PERF_HZ:-49}" -g -p "$PID" -o "$dir/perf.data" -- sleep "$PERF_S" > "$dir/perf_record.log" 2>&1 &
    perf_pid=$!
  else
    echo "perf not installed" > "$dir/perf_record.log"
  fi
  cp /proc/$PID/smaps_rollup "$dir/smaps_rollup.txt" 2>&1
  cp /proc/$PID/status "$dir/status.txt" 2>&1
  cp /proc/meminfo "$dir/meminfo.txt"; grep '^thp_' /proc/vmstat > "$dir/vmstat_thp.txt"
  { echo "wchan:"; awk 1 /proc/$PID/task/*/wchan | sort | uniq -c | sort -rn
    echo "state:"; cat /proc/$PID/task/*/stat | sed 's/^.*) \(.\) .*/\1/' | sort | uniq -c | sort -rn; } > "$dir/thread_states.txt" 2>&1
  # one line per thread (its stack, frames joined by ';'), then the distinct stacks with their thread counts
  for t in /proc/$PID/task/*; do $SUDO cat "$t/stack" 2>/dev/null | sed 's/^\[<[0-9a-f]*>\] //' | tr '\n' ';'; echo; done \
    | grep ';' | sort | uniq -c | sort -rn > "$dir/kernel_stacks_unique.txt"
  [ -s "$dir/kernel_stacks_unique.txt" ] || echo "no kernel stacks (sudo needed for /proc/PID/task/*/stack)" > "$dir/kernel_stacks_unique.txt"
  grep -a '^MAINT_TRACE ' "$TRACE" 2>/dev/null | tail -200 > "$dir/trace_last200.txt"
  tail -n 200 "$LOG" > "$dir/watchdog_tail.txt"
  if [ -n "$perf_pid" ]; then
    wait "$perf_pid"
    local report="$SUDO perf report -i $dir/perf.data --stdio --no-children"
    $report -g none --sort dso,symbol 2> /dev/null | grep -v '^#' | grep -v '^$' | cut -c1-220 | sed 's/ *$//' | head -100 > "$dir/perf_report.txt"
    $report -g caller,2,callee --percent-limit 3 --sort symbol 2> /dev/null | grep -v '^#' | cut -c1-220 | head -300 > "$dir/perf_callgraph.txt"
    $SUDO chown "$(id -u):$(id -g)" "$dir"/perf.data 2> /dev/null
  fi
  say "evidence captured: $(ls "$dir" | tr '\n' ' ')"
}

stop_run() {
  [ "${WD_KILL:-1}" = 1 ] || { say "WD_KILL=0: leaving the run alone"; return; }
  say "killing pid $PID (SIGTERM, SIGKILL after ${GRACE}s)"
  kill -TERM "$PID" 2> /dev/null
  for _ in $(seq "$GRACE"); do kill -0 "$PID" 2> /dev/null || break; sleep 1; done
  kill -0 "$PID" 2> /dev/null && kill -KILL "$PID" 2> /dev/null
  say "run killed"
}

last_sample=0
while kill -0 "$PID" 2> /dev/null; do
  now=$(date +%s)
  if [ $((now - last_sample)) -ge "$INTERVAL" ]; then sample; last_sample=$now; fi
  avail=$(meminfo_kb MemAvailable)
  if [ -n "$avail" ] && [ "$avail" -lt "$MEM_LIMIT_KB" ]; then
    capture_evidence "MemAvailable $((avail / 1048576)) GB below $((MEM_LIMIT_KB / 1048576)) GB"; stop_run; exit 3
  fi
  if [ "${WD_STALE_KILL:-1}" = 1 ] && grep -aq ' STALE ' "$TRACE" 2>/dev/null; then
    capture_evidence "an accelerator fell back to a rebuild: $(grep -a ' STALE ' "$TRACE" | head -1 | cut -d' ' -f4-)"; stop_run; exit 5
  fi
  read -r age span <<< "$(oldest_open_span)"
  if [ -n "${age:-}" ] && [ "$age" -gt "$LIMIT" ]; then
    capture_evidence "span $span open for ${age}s (limit ${LIMIT}s)"; stop_run; exit 4
  fi
  sleep "$POLL"
done
say "pid $PID exited; watchdog done"
