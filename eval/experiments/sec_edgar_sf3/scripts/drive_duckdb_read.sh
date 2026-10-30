#!/usr/bin/env bash
# Full-machine read-only sweep of the SEC-EDGAR accelerators vs DuckDB (40 vs 40): cores 0-39, DuckDB threads =
# accelerator threads = 40.  Queries 1 2 3 4 6 24, 5 fresh random draws x 8 parameter sets (one set for Q1 when it has no
# parameter), 5 timed runs per set; each (query, system) process works on a fresh copy of the database.
# SYSTEMS: cpp (C++ accelerators), verus (Verus kits on the one world store, ACCEL_AUX_STORE=verus_global).
# Writes read_q<N>_<system>_r<draw>.json, read.log, run.log, seeds.json, thread_settings.txt into OUT.
# Paper run: cpp bars = SYSTEMS=cpp on the sec_readonly tree -> data/cpp/duckdb; verified bars = SYSTEMS=verus on the
# vpg/bench tree (Verus kits) -> data/verified/duckdb (that run's cpp output is not used).  SEED is shared with the Postgres driver.
# Requires: TREE (built tree with build-release/aux_compare), DB (SEC-EDGAR SF3 .duckdb), OUT.
# Smoke test: QUERIES=6 REPEATS=1 SETS=2 ./drive_duckdb_read.sh
set -uo pipefail
TREE=${TREE:?TREE: built accelerator tree containing build-release/aux_compare}
DB=${DB:?DB: path of the SEC-EDGAR SF3 DuckDB database}
OUT=${OUT:?OUT: output directory}
cd "$TREE"
. scripts/perf/common.sh
perf_setup "$TREE" 0-39
export WORK=${WORK:-/dev/shm/bench_work} TMPDIR=${TMPDIR:-/dev/shm}
SEED=${SEED:-$(od -An -N4 -tu4 /dev/urandom | tr -d ' ')}
mkdir -p "$OUT" "$WORK"
until awk '{exit !($1 < ('"${MAX_LOAD:-2}"'))}' /proc/loadavg && ! pgrep -x cc1plus >/dev/null && ! pgrep -x ninja >/dev/null; do sleep 30; done
perf_record_settings "$OUT"
echo "=== start $(date +%T) commit $(git rev-parse --short HEAD) seed $SEED db $DB load $(cut -d' ' -f1 /proc/loadavg)" | tee -a "$OUT/run.log"
$RUN read --workload sec_edgar --db "$DB" --sf 3 --systems "${SYSTEMS:-cpp verus}" --copy --out "$OUT" --bin "$TREE/build-release/aux_compare" \
  --repeats "${REPEATS:-5}" --sets "${SETS:-8}" --seed "$SEED" --queries "${QUERIES:-1 2 3 4 6 24}" 2>&1 | tee -a "$OUT/run.log"
rc=${PIPESTATUS[0]}
echo "=== finished $(date +%T) rc=$rc" | tee -a "$OUT/run.log"
echo "rc=$rc" > "$OUT/finished"
rm -rf "$WORK"
