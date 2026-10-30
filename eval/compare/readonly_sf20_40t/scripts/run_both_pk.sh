#!/usr/bin/env bash
# SF20 read-only runs on socket 0 only (CPUs 0-19 + HT 40-59, memory node 0) = one Xeon Gold 6230.
set -uo pipefail
ulimit -c 0   # no core dumps: systemd-coredump writes them to / (nearly full)
REPO=/spinning/akhilvb/accel_sf20/accelerator-codegen
CORES=0-19,40-59; T=40
export ACCEL_DUCKDB_ROOT=/spinning/akhilvb/accel_sf20/accelerator-codegen-duckdb TMPDIR=/spinning/akhilvb/accel_sf20/tmp
DB=/spinning/akhilvb/accel_sf20/tpch_sf20_pk.duckdb; WORK=/ssd1/akhilvb/accel_sf20_work; O=/spinning/akhilvb/accel_sf20/results_40t_socket0_pk
wait_quiet() {
  while :; do
    l=$(cut -d' ' -f1 /proc/loadavg)
    if awk -v l="$l" 'BEGIN{exit !(l < 2)}' && ! pgrep -x 'cc1plus|cc1|rustc|ld|ld.lld|ninja|make|verus|z3' >/dev/null; then break; fi
    echo "$(date +%T) waiting for quiet machine (load $l)"; sleep 30
  done
  echo "$(date +%T) quiet: load $(cut -d' ' -f1 /proc/loadavg)"
}
cd $REPO
echo "== host: $(nproc) CPUs visible; running on $CORES (T=$T), membind node 0"; lscpu | grep -E '^Socket|Core\(s\) per|Thread\(s\) per'
wait_quiet
echo "== A. Verus 1 thread vs DuckDB $T threads $(date +%T)"
DB=$DB WORK=$WORK MAX_LOAD=2 numactl --membind=0 scripts/perf/run_sf20_read.sh $REPO $CORES $O/verus_read --systems verus --seed 632608726
echo "A rc=$? $(date +%T)"
wait_quiet
echo "== B. Stateless C++ $T vs $T (and 1 vs 1) $(date +%T)"
SF=20 ACCEL_TPCH_DB=$DB OUT_DIR=$O/stateless numactl --membind=0 taskset -c $CORES scripts/run_stateless_duckdb.sh $T 20
echo "B rc=$? $(date +%T)"
echo ALL DONE
