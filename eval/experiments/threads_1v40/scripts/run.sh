#!/usr/bin/env bash
# Read latency of the stateful C++ accelerators with 1 vs 40 read threads (Q1, Q3, Q13, Q18, Q21, SF20).
# Writes data/q<N>_t<T>.json (aux_compare read output, 7 reps over the cases in data/cases).
# The original run used a build from a fix_conc_decision tree (verus-duckdb build of aux_compare).
# Required: AUX_COMPARE (aux_compare binary), DB (SF20 TPC-H DuckDB database).
set -uo pipefail
REPO=${REPO:-$(git rev-parse --show-toplevel)}
HERE=$(cd "$(dirname "$0")/.." && pwd)
: "${AUX_COMPARE:?path to the aux_compare binary}" "${DB:?path to the SF20 TPC-H DuckDB database}"
CASES=${CASES:-$HERE/data/cases}
OUT=${OUT:-$HERE/data}
export TMPDIR=${TMPDIR:-/dev/shm} ACCEL_AUX_THREADS=40 ACCEL_AUX_BUILD_THREADS=40 ACCEL_AUX_MAINT_THREADS=8 \
  ACCEL_COMPARE_DUCKDB_THREADS=40 ACCEL_COMPARE_DUCKDB_BUILD_THREADS=40
for q in 1 3 13 18 21; do
  for t in 1 40; do
    ACCEL_AUX_READ_THREADS=$t "$AUX_COMPARE" read "$DB" tpch:$q tpch_q${q}_vgen "$CASES/cases_q${q}_cpp_r0.tsv" 7 \
      > "$OUT/q${q}_t${t}.json" 2> "$OUT/q${q}_t${t}.err"
    echo "q$q t$t exit $?"
  done
done
