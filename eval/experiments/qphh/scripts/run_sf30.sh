#!/usr/bin/env bash
# TPC-H performance test at SF30 (power + throughput, 4 streams, 3 runs): DuckDB vs the C++ stateful accelerators.
# Writes tpch_{base,cpp}_r{0,1,2}.json, run.log, seeds.json, thread_settings.txt into OUT (default: ../data/sf30).
# The original run used tree commit 50641f0e on a 40-core box, cores 0-39.
# Required: DB (DuckDB TPC-H SF30 database), RF_DIR (SF30 refresh sets).
set -euo pipefail
REPO=${REPO:-$(git rev-parse --show-toplevel)}
OUT=${OUT:-$(dirname "$0")/../data/sf30}
: "${DB:?path to the SF30 TPC-H DuckDB database}" "${RF_DIR:?path to the SF30 refresh sets}"
mkdir -p "$OUT"
cd "$REPO"
DB=$DB RF_DIR=$RF_DIR RUNS=3 STREAMS=4 TMPDIR=${TMPDIR:-/dev/shm} \
  scripts/perf/run_qphh.sh "$REPO" 30 0-39 "$OUT" > "$OUT/console.log" 2>&1
