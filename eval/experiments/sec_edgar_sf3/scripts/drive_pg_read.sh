#!/usr/bin/env bash
# Full-machine read-only sweep of the SEC-EDGAR accelerators vs plain Postgres (40 vs 40): the server container pinned to
# cores 0-39, every thread cap = 40 as server GUCs (max_parallel_workers_per_gather, max_parallel_workers, accel.*_threads;
# scripts/perf/pg_sf20/set_gucs.sh, run by run_sweep.sh, restarts the server and records them in thread_settings.txt).
# Queries 1 2 3 4 6 24, 5 draws x 8 sets (one for Q1), accelerator 5 timed calls per set, plain Postgres 1 timed run per set.
# SYSTEMS: cpp (C++ accelerators), verus (Verus kits on the world store; the server runs with ACCEL_AUX_STORE=verus_global).
# Writes cases.csv, qstats.jsonl, run.log, seeds.json, session_gucs.txt, thread_settings.txt into OUT/<system>.
# Paper run: cpp bars = the sec_readonly tree (one system, output straight in OUT) -> data/cpp/pg; verified bars = the vpg/bench
# tree, system verus -> data/verified/pg (that run's cpp output is not used).  Both systems get the same SEED.
# Requires: TREE (accelerator tree), OUT, and the Postgres engine environment (ACCEL_ENGINE_STATE_DIR etc.) exported,
# with the container T listening on PG_PORT (read from $ACCEL_ENGINE_STATE_DIR/env when unset).
# Smoke test: QUERIES=6 REPEATS=1 SETS=2 ./drive_pg_read.sh
set -uo pipefail
TREE=${TREE:?TREE: accelerator tree with scripts/perf/pg_sf20/run_sweep.sh}
OUT=${OUT:?OUT: output directory}
: "${ACCEL_ENGINE_STATE_DIR:?ACCEL_ENGINE_STATE_DIR: Postgres engine state dir (export the engine environment first)}"
BASE_OUT=$OUT
SEED=${SEED:-$(od -An -N4 -tu4 /dev/urandom | tr -d ' ')}
export T=${T:-accel_pg_sec_edgar} DB=${DB:-accel_sec_edgar} CORES=0-39
PG_PORT=${PG_PORT:-$(sed -n 's/^PGPORT=//p' "$ACCEL_ENGINE_STATE_DIR/env")}
for system in ${SYSTEMS:-cpp verus}; do
  sysout=$BASE_OUT/$system
  mkdir -p "$sysout"
  export OUT=$sysout
  echo "=== start $system $(date +%T) commit $(git -C "$TREE" rev-parse --short HEAD) seed $SEED cores $CORES container $T load $(cut -d' ' -f1 /proc/loadavg)" | tee -a "$sysout/run.log"
  "$TREE/scripts/perf/pg_sf20/run_sweep.sh" --workload sec_edgar --system "$system" --db "$DB" --port "$PG_PORT" --sf 3 \
    --repeats "${REPEATS:-5}" --sets "${SETS:-8}" --seed "$SEED" --queries "${QUERIES:-1 2 3 4 6 24}" 2>&1 | tee -a "$sysout/run.log"
  rc=${PIPESTATUS[0]}
  echo "=== finished $system $(date +%T) rc=$rc" | tee -a "$sysout/run.log"
  echo "rc=$rc" > "$sysout/finished"
done
