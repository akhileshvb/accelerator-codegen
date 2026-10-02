#!/usr/bin/env bash
# DuckDB accelerators end to end: base kernels (test_tpch_q<N>), aux state
# (shared differential per query, incremental differential over both
# workloads), table-function registration, and the Verus Q21 accelerators.
# usage: scripts/run_duckdb_registry_checks.sh [build_dir]
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
B="${1:-${ROOT}/build-verus-duckdb}"
DATA="${ACCEL_DUCKDB_DATA:-/home/akhilvb/accelerator-codegen-duckdb}"
TPCH="${ACCEL_TPCH_AUX_DB:-${DATA}/tpch_sf1.duckdb}"
SEC="${DATA}/sec_edgar_test.duckdb"
SF1="${DATA}/tpch_sf1.duckdb"
SMALL="${DATA}/tpch_test.duckdb"
WORK="$(mktemp -d)"
pass=0
fail=0
failed=()

run() {
  local name="$1"
  shift
  if out=$("$@" 2>&1); then
    pass=$((pass + 1))
    echo "PASS ${name}"
  else
    fail=$((fail + 1))
    failed+=("${name}")
    echo "FAIL ${name}"
    echo "${out}" | tail -8
  fi
}

ONLY="${ACCEL_CHECKS_ONLY:-base,aux,incremental,verus}"
want() { [[ ",${ONLY}," == *",$1,"* ]]; }

want base && [[ -x "${B}/test_registration" ]] && run registration env ACCEL_TPCH_DB="${SF1}" "${B}/test_registration"
want base && for q in $(seq 1 22); do
  [[ -x "${B}/test_tpch_q${q}" ]] && run "base_q${q}" env ACCEL_TPCH_DB="${SF1}" "${B}/test_tpch_q${q}"
done
want aux && for q in $(seq 1 22); do
  run "aux_q${q}" "${B}/test_tpch_shared_differential" "${TPCH}" "${q}"
done
want incremental && run incremental "${B}/test_shared_incremental_differential" "${TPCH}" "${SEC}"
if want verus && [[ -x "${B}/verus_kit_runner" ]]; then
  cp "${SMALL}" "${WORK}/kit.duckdb"
  run verus_kit_q21 "${B}/verus_kit_runner" differential "${WORK}/kit.duckdb" tpch_q21_verus_kit \
    "${ROOT}/tests/verification/verus_kit/q21_spec"
fi
if want verus && [[ -x "${B}/q21_verus_duckdb_snapshots" ]]; then
  cp "${SMALL}" "${WORK}/snap.duckdb"
  run verus_q21_snapshots "${B}/q21_verus_duckdb_snapshots" "${WORK}/snap.duckdb" 20 2 7
fi
echo "summary pass=${pass} fail=${fail} ${failed[*]:-}"
[[ ${fail} -eq 0 ]]
