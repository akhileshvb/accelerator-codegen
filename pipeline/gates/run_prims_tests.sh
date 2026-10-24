#!/usr/bin/env bash
# Gate: the shared primitives library (accelerators/prims/) builds, passes its unit and
# randomized reference tests, only grew relative to HEAD, and names no workload.
# Standalone (header-only, no DBMS): builds test_prims from pipeline/gates/prims_test.
# usage: run_prims_tests.sh [query_number]
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ACCEL_PRIMS_BUILD_DIR:-${ROOT}/build-prims}"
if [[ -n "${QUERY}" ]]; then
  # shellcheck source=aux_gate_common.sh
  source "${HERE}/aux_gate_common.sh"
  load_aux_gate_context "${ROOT}" "${QUERY}"
  OUT="${GATE_OUT}"
else
  OUT="${ROOT}/pipeline/artifacts/shared/gates"
fi
mkdir -p "${OUT}"

LOG=""
STATUS="pass"
REASON=""
step() { # step <label> <cmd...>; records the first failure
  [[ "${STATUS}" == "pass" ]] || return 0
  local label="$1"
  shift
  local out rc
  out="$("$@" 2>&1)"
  rc=$?
  if [[ ${rc} -ne 0 ]]; then
    STATUS="fail"
    REASON="${label}"
    LOG+="[${label}]"$'\n'"${out}"$'\n'
  fi
}

step "workload words in prims/" python3 "${ROOT}/scripts/check_backend_query_free.py" --prims-only
step "prims changed or removed existing lines" python3 "${HERE}/check_prims_additions.py" "${ROOT}"
step "configure test_prims" cmake -S "${ROOT}/pipeline/gates/prims_test" -B "${BUILD}" \
  -DPRIMS_REPO_ROOT="${ROOT}" -DCMAKE_BUILD_TYPE=Release
step "build test_prims" cmake --build "${BUILD}" --target test_prims -j "${ACCEL_BUILD_JOBS:-$(nproc)}"
step "run test_prims" "${BUILD}/test_prims"

PRIMS_LOG="${LOG}" PRIMS_STATUS="${STATUS}" PRIMS_REASON="${REASON}" PRIMS_OUT="${OUT}" \
  PRIMS_QUERY="${QUERY:-0}" python3 - <<'PY'
import json, os, pathlib
out = pathlib.Path(os.environ["PRIMS_OUT"]) / "prims_tests.json"
payload = {
    "gate": "prims_tests",
    "query": int(os.environ["PRIMS_QUERY"]),
    "status": os.environ["PRIMS_STATUS"],
    "reason": os.environ["PRIMS_REASON"],
    "log_tail": os.environ["PRIMS_LOG"][-4000:],
}
out.write_text(json.dumps(payload, indent=2))
print(out.read_text())
PY

[[ "${STATUS}" == "pass" ]]
