#!/usr/bin/env bash
# Fast correctness-only check for all shared-interface DuckDB queries.
# Phase 1: cold-build differential. Phase 2: incremental maintenance differential.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD:-${ROOT}/build}"
BENCH_BIN="${BIN:-${BUILD}/benchmark_shared_duckdb_write_interval}"
INC_BIN="${INC_BIN:-${BUILD}/test_shared_incremental_differential}"
TPCH_DB="${TPCH_DB:-${ROOT}/../accelerator-codegen-duckdb/tpch_sf1.duckdb}"
TPCH_TEST_DB="${TPCH_TEST_DB:-${ROOT}/../accelerator-codegen-duckdb/tpch_test.duckdb}"
SEC_EDGAR_DB="${SEC_EDGAR_DB:-${ROOT}/../accelerator-codegen-duckdb/sec_edgar_sf3.duckdb}"
SEC_EDGAR_TEST_DB="${SEC_EDGAR_TEST_DB:-${ROOT}/../accelerator-codegen-duckdb/sec_edgar_test.duckdb}"

if [[ ! -x "$BENCH_BIN" ]]; then
  echo "missing benchmark binary: $BENCH_BIN" >&2
  exit 1
fi

export ACCEL_SHARED_SWEEP_RUN_DIFFERENTIAL=1
export ACCEL_SHARED_SWEEP_ONLY_DIFFERENTIAL=1

failures=0
run_one() {
  local workload="$1"
  local query="$2"
  local db="$3"
  echo "=== duckdb ${workload} Q${query} (cold differential) ==="
  if "$BENCH_BIN" "$workload" "$query" "$db" 0; then
    echo "PASS"
  else
    echo "FAIL"
    failures=$((failures + 1))
  fi
}

for q in 1 3 6 10 12 14; do
  run_one tpch "$q" "$TPCH_TEST_DB"
done
for q in 1 2 3 4 6 24; do
  run_one sec_edgar "$q" "$SEC_EDGAR_TEST_DB"
done

if [[ "$failures" -ne 0 ]]; then
  echo "${failures} cold differential check(s) failed" >&2
  exit 1
fi
echo "all 12 DuckDB cold differential checks passed"

if [[ ! -x "$INC_BIN" ]]; then
  echo "skipping incremental differential: missing $INC_BIN" >&2
  exit 0
fi
if [[ ! -f "$TPCH_TEST_DB" || ! -f "$SEC_EDGAR_TEST_DB" ]]; then
  echo "skipping incremental differential: database file(s) missing" >&2
  exit 0
fi

echo "=== duckdb incremental differential (all 12) ==="
if "$INC_BIN" "$TPCH_TEST_DB" "$SEC_EDGAR_TEST_DB"; then
  echo "all 12 DuckDB incremental differential checks passed"
else
  echo "incremental differential check failed" >&2
  exit 1
fi
