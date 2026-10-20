#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_profile.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

JSON="${GATE_OUT}/profile.json"
if ! engine_ready "${BENCH_TARGET}"; then
  emit_result "${JSON}" profile skip "benchmark target not built: ${ENGINE_FAIL}" "${ENGINE_LOG}" "query:=${QUERY}"
fi

ACCEL_BENCH_PROFILE=1 engine_run "${BENCH_TARGET}" "${QUERY}"
SUMMARY="${GATE_ART}/profile_summary.json"
python3 -c "import json,sys; json.dump({'log_tail': sys.argv[1][-8000:]}, open(sys.argv[2], 'w'), indent=2)" "${LOG}" "${SUMMARY}"
STATUS=pass
[[ ${RC} -eq 0 ]] || STATUS=fail
emit_result "${JSON}" profile "${STATUS}" "" "${ENGINE_LOG}${LOG}" "query:=${QUERY}" "exit_code:=${RC}" "profile_summary=${SUMMARY}"
