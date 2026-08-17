#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-${ROOT}/build-release}}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_correctness.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

OUT="${GATE_OUT}"
mkdir -p "${OUT}"

# shellcheck source=cmake_ensure_target.sh
source "$(dirname "${BASH_SOURCE[0]}")/cmake_ensure_target.sh"

if ! ensure_build_tree "${ROOT}" "${BUILD}"; then
  echo "{\"gate\":\"aux_correctness\",\"query\":${QUERY},\"status\":\"fail\",\"reason\":\"cmake configure failed for ${BUILD}\"}" > "${OUT}/aux_correctness.json"
  exit 1
fi

BUILD_LOG=""
if ! ensure_cmake_target "${ROOT}" "${BUILD}" "${AUX_TEST_TARGET}"; then
	BUILD_LOG="$(cmake --build "${BUILD}" --target "${AUX_TEST_TARGET}" -j"$(nproc)" 2>&1 | tail -40 || true)"
fi

if [[ ! -x "${BUILD}/${AUX_TEST_TARGET}" ]]; then
	python3 - <<PY
import json, pathlib, sys
path = pathlib.Path("${OUT}/aux_correctness.json")
payload = {
    "gate": "aux_correctness",
    "query": int("${QUERY}"),
    "benchmark": "${BENCHMARK_NAME}",
    "status": "fail",
    "reason": "${AUX_TEST_TARGET} missing after cmake configure/build",
    "log_tail": """${BUILD_LOG}"""[-4000:],
}
path.write_text(json.dumps(payload, indent=2))
print(path.read_text())
sys.exit(1)
PY
fi

if [[ -x "${BUILD}/ctest" ]]; then
  CTEST="${BUILD}/ctest"
else
  CTEST="ctest"
fi

set +e
SMOKE_LOG="$(cd "${BUILD}" && env "${DB_ENV_VAR}=${DATASET_DB}" "${CTEST}" -R "${AUX_CTEST_SMOKE_REGEX}" --output-on-failure 2>&1)"
SMOKE_RC=$?
set -e

STATUS="pass"
REASON=""
DIFF_LOG=""
DIFF_RC=0
LOG="${SMOKE_LOG}"

if [[ ${SMOKE_RC} -ne 0 ]]; then
  STATUS="fail"
  REASON="aux smoke unit test failed"
else
  # SF1 DuckDB reference SQL vs aux Execute (catalog seed binding).
  # Catches predicate / eligibility bugs that synthetic fixtures can miss or invent wrongly.
  if [[ "${BENCHMARK_NAME}" == "tpch" ]]; then
    if ! ensure_cmake_target "${ROOT}" "${BUILD}" "test_tpch_shared_differential"; then
      DIFF_LOG="$(cmake --build "${BUILD}" --target test_tpch_shared_differential -j"$(nproc)" 2>&1 | tail -60 || true)"
    fi
    if [[ ! -x "${BUILD}/test_tpch_shared_differential" ]]; then
      STATUS="fail"
      REASON="test_tpch_shared_differential missing after build"
      LOG="${SMOKE_LOG}"$'\n'"${DIFF_LOG}"
      DIFF_RC=1
    elif [[ ! -f "${DATASET_DB}" ]]; then
      STATUS="fail"
      REASON="dataset db missing for SF1 differential: ${DATASET_DB}"
      DIFF_RC=1
      LOG="${SMOKE_LOG}"$'\n'"${REASON}"
    else
      set +e
      # Optional adversarial scenario contract from post-analysis stage.
      SCENARIOS="${GATE_ART}/adversarial_write_scenarios.json"
      unset ACCEL_ADV_MAX_PROBES ACCEL_ADV_MIN_CHUNKS ACCEL_ADV_REQUIRE_BULK || true
      if [[ -f "${SCENARIOS}" ]]; then
        ACCEL_ADV_MAX_PROBES="$(python3 -c "import json,pathlib;d=json.loads(pathlib.Path('${SCENARIOS}').read_text());p=d.get('point_writes') or {};print(int(p.get('max_probes') or p.get('min_count') or 5))")"
        ACCEL_ADV_MIN_PROBES="$(python3 -c "import json,pathlib;d=json.loads(pathlib.Path('${SCENARIOS}').read_text());p=d.get('point_writes') or {};print(int(p.get('min_count') or 2))")"
        ACCEL_ADV_MIN_CHUNKS="$(python3 -c "import json,pathlib;d=json.loads(pathlib.Path('${SCENARIOS}').read_text());b=d.get('bulk_writes') or {};print(int(b.get('min_chunks') or 0))")"
        ACCEL_ADV_REQUIRE_BULK="$(python3 -c "import json,pathlib;d=json.loads(pathlib.Path('${SCENARIOS}').read_text());b=d.get('bulk_writes') or {};print(1 if b.get('required') else 0)")"
        export ACCEL_ADV_MAX_PROBES ACCEL_ADV_MIN_PROBES ACCEL_ADV_MIN_CHUNKS ACCEL_ADV_REQUIRE_BULK
      fi
      DIFF_LOG="$(cd "${BUILD}" && env "${DB_ENV_VAR}=${DATASET_DB}" \
        ./test_tpch_shared_differential "${DATASET_DB}" "${QUERY}" 2>&1)"
      DIFF_RC=$?
      set -e
      LOG="${SMOKE_LOG}"$'\n'"${DIFF_LOG}"
      if [[ ${DIFF_RC} -ne 0 ]]; then
        STATUS="fail"
        REASON="SF1 SQL-vs-aux differential failed"
      fi
    fi
  fi
fi

python3 - <<PY
import json, pathlib
path = pathlib.Path("${OUT}/aux_correctness.json")
path.write_text(json.dumps({
    "gate": "aux_correctness",
    "query": int("${QUERY}"),
    "benchmark": "${BENCHMARK_NAME}",
    "status": "${STATUS}",
    "exit_code": int("${SMOKE_RC}") if "${STATUS}" == "pass" or "${REASON}" == "aux smoke unit test failed" else int("${DIFF_RC}"),
    "smoke_exit_code": int("${SMOKE_RC}"),
    "differential_exit_code": int("${DIFF_RC}"),
    "reason": """${REASON}""",
    "log_tail": """${LOG}"""[-6000:],
}, indent=2))
print(path.read_text())
PY

[[ "${STATUS}" == "pass" ]]
