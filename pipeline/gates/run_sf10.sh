#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_sf10.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

JSON="${GATE_OUT}/sf10.json"
SCALE_DIR="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('scale_dataset_dir',''))" "${CTX}")"
if [[ -z "${SCALE_DIR}" || ! -d "${SCALE_DIR}" ]]; then
  emit_result "${JSON}" sf10 skip "no scale dataset directory configured (scale_dataset_dir)" "" "query:=${QUERY}"
fi

# A separate instance and dataset: the scale run never disturbs the SF1 instance.
export ACCEL_ENGINE_INSTANCE="${ACCEL_ENGINE_INSTANCE}-sf10"
export ACCEL_ENGINE_STATE_DIR="${ACCEL_ENGINE_STATE_DIR}-sf10"
export ACCEL_DATASET_DIR="${SCALE_DIR}"
mkdir -p "${ACCEL_ENGINE_STATE_DIR}"
if ! engine_ready "${BENCH_TARGET}"; then
  emit_result "${JSON}" sf10 skip "benchmark target not built: ${ENGINE_FAIL}" "${ENGINE_LOG}" "query:=${QUERY}"
fi
engine_run "${BENCH_TARGET}" "${QUERY}"
STATUS=pass
[[ ${RC} -eq 0 ]] || STATUS=fail
emit_result "${JSON}" sf10 "${STATUS}" "" "${ENGINE_LOG}${LOG}" "query:=${QUERY}" "exit_code:=${RC}"
