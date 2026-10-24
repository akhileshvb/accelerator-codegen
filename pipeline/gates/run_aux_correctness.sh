#!/usr/bin/env bash
# Aux-state correctness: the aux smoke test, then the SQL-vs-aux differential (tpch),
# both built and run through the engine host. ACCEL_GATE_TIER=quick runs it on the subset
# dataset (ACCEL_SUBSET_DATASET_DIR) with at most ACCEL_QUICK_PROBES write probes; the default
# full tier is the stage-exit check. Run as the gate aux_correctness_quick, its result never overwrites
# aux_correctness.json.
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
source "$(dirname "${BASH_SOURCE[0]}")/suite_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

JSON="${GATE_OUT}/${ACCEL_GATE_NAME:-aux_correctness}.json"
WITH_DIFF=0
[[ "${BENCHMARK_NAME}" == "tpch" ]] && WITH_DIFF=1
if [[ -z "${AUX_TEST_TARGET}" ]] || { [[ ${WITH_DIFF} -eq 1 && -z "${DIFF_BINARY}" ]]; }; then
  emit_result "${JSON}" aux_correctness fail "aux test / differential target missing from the gate context. ${HOST_HINT}" ""
fi

TARGETS=("${AUX_TEST_TARGET}")
[[ ${WITH_DIFF} -eq 1 ]] && TARGETS+=("${DIFF_BINARY}")
if ! engine_ready "${TARGETS[@]}"; then
  emit_result "${JSON}" aux_correctness fail "${ENGINE_FAIL}" "${ENGINE_LOG}"
fi

engine_run "${AUX_TEST_TARGET}"
SMOKE_RC=${RC}
LOG_ALL="${ENGINE_LOG}${LOG}"
if [[ ${SMOKE_RC} -ne 0 ]]; then
  emit_result "${JSON}" aux_correctness fail "aux smoke unit test failed" "${LOG_ALL}" \
    "differential_binary=${DIFF_BINARY}" "smoke_exit_code:=${SMOKE_RC}"
fi

DIFF_RC=0
if [[ ${WITH_DIFF} -eq 1 ]]; then
  suite_prepare "${ACCEL_GATE_TIER:-full}" "${JSON}" "${ACCEL_GATE_NAME:-aux_correctness}"
  SCENARIOS="${ACCEL_SUITE_DIR:-}/suite.yaml"
  if [[ -f "${SCENARIOS}" ]]; then
    eval "$(QUICK_PROBES="${ACCEL_QUICK_PROBES:-3}" python3 - "${SCENARIOS}" <<'PY'
import os, sys, yaml
d = yaml.safe_load(open(sys.argv[1]))
p, b = d.get("point_writes") or {}, d.get("bulk_writes") or {}
cap = int(os.environ["QUICK_PROBES"]) if os.environ.get("ACCEL_GATE_TIER") == "quick" else 1 << 30
print(f"export ACCEL_ADV_MAX_PROBES={min(cap, int(p.get('max_probes') or p.get('min_count') or 5))}")
print(f"export ACCEL_ADV_MIN_PROBES={min(cap, int(p.get('min_count') or 2))}")
print(f"export ACCEL_ADV_MIN_CHUNKS={int(b.get('min_chunks') or 0)}")
print(f"export ACCEL_ADV_REQUIRE_BULK={1 if b.get('required') else 0}")
PY
)"
  fi
  [[ "${ACCEL_GATE_TIER:-full}" != quick ]] || export ACCEL_ADV_MAX_PROBES="${ACCEL_ADV_MAX_PROBES:-${ACCEL_QUICK_PROBES:-3}}"
  engine_run "${DIFF_BINARY}" "${QUERY}"
  DIFF_RC=${RC}
  LOG_ALL="${LOG_ALL}"$'\n'"${LOG}"
  suite_extras run_aux_correctness.sh
  if [[ ${DIFF_RC} -ne 0 ]]; then
    emit_result "${JSON}" aux_correctness fail "SQL-vs-aux differential failed (${DIFF_BINARY})" "${LOG_ALL}" \
      "differential_binary=${DIFF_BINARY}" "smoke_exit_code:=${SMOKE_RC}" "differential_exit_code:=${DIFF_RC}" "${SUITE_EXTRAS[@]}"
  fi
fi

# The query must register from its own directory: the engine backend names no query.
CHECK_STARTED="$(now_us)"
BACKEND_RC=0
BACKEND_LOG="$(python3 "${ROOT}/scripts/check_backend_query_free.py" --engine "${ENGINE_NAME}" 2>&1)" || BACKEND_RC=$?
record_phase checker "${CHECK_STARTED}"
if [[ ${BACKEND_RC} -ne 0 ]]; then
  emit_result "${JSON}" aux_correctness fail \
    "per-query code in the ${ENGINE_NAME} backend: register from accelerators/${BENCHMARK_NAME}/q${QUERY}/ instead" \
    "${BACKEND_LOG}" "differential_binary=${DIFF_BINARY}"
fi
emit_result "${JSON}" aux_correctness pass "" "${LOG_ALL}" "differential_binary=${DIFF_BINARY}" \
  "smoke_exit_code:=${SMOKE_RC}" "differential_exit_code:=${DIFF_RC}" "${SUITE_EXTRAS[@]}"
