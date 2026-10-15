# Slot gate adapter: DuckDB. The generic scenarios run in-process through
# eval/tests/test_slot_gates_duckdb (the DuckDB driver of eval/harness/slot_gates.hpp).
# Sourced by run_slot_gates.sh, which provides ROOT and BUILD_DIR.

_duckdb_build() { # target...
  [[ -f "${BUILD_DIR}/CMakeCache.txt" ]] || cmake -S "${ROOT}" -B "${BUILD_DIR}" || return 1
  cmake --build "${BUILD_DIR}" --target "$@" -j"$(nproc)" || return 1
}

# Every TPC-H query's aux differential (read, incremental and adversarial
# writes) and the RF differential, on a private copy of the database.
_duckdb_differential() {
  local db="${ACCEL_TPCH_DB:-${ROOT}/../accelerator-codegen-duckdb/tpch_sf1.duckdb}"
  _duckdb_build test_tpch_shared_differential test_shared_tpch_rf_differential || return 1
  local failed=0 q
  for q in ${ACCEL_SLOT_DIFF_QUERIES:-$(seq 1 22)}; do
    if "${BUILD_DIR}/test_tpch_shared_differential" "${db}" "${q}" >/dev/null 2>&1; then
      echo "PASS differential Q${q}"
    else
      echo "FAIL differential Q${q}"
      failed=1
    fi
  done
  local work
  work="$(mktemp -d)" || return 1
  cp "${db}" "${work}/tpch.duckdb"
  if "${BUILD_DIR}/test_shared_tpch_rf_differential" "${work}/tpch.duckdb"; then
    echo "PASS rf differential"
  else
    echo "FAIL rf differential"
    failed=1
  fi
  rm -rf "${work}"
  return "${failed}"
}

slot_adapter_run() { # scenario seconds
  local scenario="$1" seconds="$2"
  if [[ "${scenario}" == "differential" ]]; then
    _duckdb_differential
    return
  fi
  _duckdb_build test_slot_gates_duckdb || return 1
  "${BUILD_DIR}/test_slot_gates_duckdb" "${scenario}" "${seconds}"
}
