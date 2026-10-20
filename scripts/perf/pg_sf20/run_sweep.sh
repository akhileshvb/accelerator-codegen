#!/usr/bin/env bash
# Timed sweep: waits for a quiet machine, pins the server container to CORES, holds the lock file while timing,
# restores the pinning on exit, then writes <out>/summary.md. Extra arguments go to sweep.py (--queries, --repeats, --sets ...).
# Every thread setting (Postgres workers, accel.threads and its read/build/maint caps) is the core count of CORES;
# the server's verified settings go to <out>/thread_settings.txt.
# Env: OUT (required), CORES (32-39), T (container), GATES (space-separated files that must exist first),
#      LOCK (lock file other agents watch, default $OUT/../pg_read.running).
#      MAX_LOAD (1-minute load the quiet gate requires below, default 2).
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT=${OUT:?set OUT}; T=${T:-accel_pg_sf20}; CORES=${CORES:-32-39}
export N=$(python3 -c 'import sys; print(sum(int(b or a) - int(a) + 1 for a, _, b in (p.partition("-") for p in sys.argv[1].split(","))))' "$CORES")
LOCK=${LOCK:-$(dirname "$OUT")/pg_read.running}
trap 'rm -f "$LOCK"; docker update --cpuset-cpus "" $T >/dev/null 2>&1' EXIT
quiet() { for g in ${GATES:-}; do [ -e "$g" ] || return 1; done; awk -v m="${MAX_LOAD:-2}" '{exit !($1<m)}' /proc/loadavg && ! pgrep -x 'cc1plus|ninja' >/dev/null; }
until quiet; do sleep 20; done
docker update --cpuset-cpus "$CORES" $T
mkdir -p "$OUT"
"$HERE/set_gucs.sh" > "$OUT/thread_settings.txt"
touch "$LOCK"
python3 -B "$HERE/sweep.py" --out "$OUT" "$@" && python3 -B "$HERE/summarize.py" "$OUT" > "$OUT/summary.md"
