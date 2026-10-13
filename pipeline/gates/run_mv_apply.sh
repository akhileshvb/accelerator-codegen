#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_mv_apply.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=mv_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/mv_gate_common.sh"
load_mv_gate_context "${ROOT}" "${QUERY}"
mkdir -p "${GATE_OUT}"

set +e
LOG="$(bash "${ROOT}/scripts/postgres_matview_apply.sh" "${QUERY}" 2>&1)"
RC=$?
set -e

if [[ ${RC} -eq 0 ]]; then
  write_mv_gate_json "mv_apply" "pass" "DDL applied" "${QUERY}"
  echo "${LOG}"
  exit 0
fi

write_mv_gate_json "mv_apply" "fail" "apply failed: ${LOG:0:500}" "${QUERY}"
echo "${LOG}" >&2
exit 1
