#!/usr/bin/env bash
# Backend-generic slot gates (engine_backend validation). One script serves every
# slot_* gate: the gate name picks the scenario, the engine's adapter runs it.
#
#   slot_epoch_stress        writers + readers at once, exact at each reader's snapshot
#   slot_hits_under_writes   continuous commits, exact answers, minimum hit rate
#                            (every isolation level the engine has)
#   slot_read_after_commit   another connection reading right after a commit sees it
#   slot_txn_edges           aborts, savepoints, TRUNCATE
#   slot_restart             accelerator workers / processes restarted under load
#   slot_differential        the 22-query differential and the RF differential
#
# An adapter (slot_adapters/<engine>.sh) defines slot_adapter_run <scenario> <seconds>
# and exits 0 (pass), 1 (fail) or 77 (the engine has no such thing: gate skipped).
# See slot_adapters/README.md. Env: ACCEL_SLOT_GATE_SECONDS (load duration, default 6).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export BUILD_DIR="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-${ROOT}/build-release}}"
GATE="${ACCEL_GATE_NAME:-slot_epoch_stress}"
OUT_JSON="${ACCEL_GATE_OUT:-/tmp/${GATE}.json}"
SCENARIO="${GATE#slot_}"
SECONDS_PER_SCENARIO="${ACCEL_SLOT_GATE_SECONDS:-6}"

ENGINE="${ACCEL_ENGINE_NAME:-}"
if [[ -n "${ACCEL_GATE_CONTEXT:-}" && -f "${ACCEL_GATE_CONTEXT}" ]]; then
  ENGINE="$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('engine_name') or '')" "${ACCEL_GATE_CONTEXT}")"
fi
ADAPTER="${ROOT}/pipeline/gates/slot_adapters/${ENGINE}.sh"

write_result() { # status notes log
  python3 - "${OUT_JSON}" "${GATE}" "${ENGINE}" "$1" "$2" "$3" <<'PY'
import json, sys
out, gate, engine, status, notes, log = sys.argv[1:7]
json.dump({"gate": gate, "engine": engine, "status": status, "notes": notes,
           "log_tail": log[-4000:]}, open(out, "w"), indent=2)
PY
}

mkdir -p "$(dirname "${OUT_JSON}")"
if [[ -z "${ENGINE}" || ! -f "${ADAPTER}" ]]; then
  write_result fail "no slot adapter for engine '${ENGINE}': add pipeline/gates/slot_adapters/${ENGINE:-<engine>}.sh (see slot_adapters/README.md)" ""
  exit 1
fi

# shellcheck source=/dev/null
source "${ADAPTER}"
LOG="$(slot_adapter_run "${SCENARIO}" "${SECONDS_PER_SCENARIO}" 2>&1)"
RC=$?
echo "${LOG}"
case "${RC}" in
  0)  write_result pass "${SCENARIO}: ${ENGINE} passed" "${LOG}" ;;
  77) write_result skip "${SCENARIO}: not applicable to ${ENGINE}" "${LOG}"; exit 0 ;;
  *)  write_result fail "${SCENARIO}: ${ENGINE} failed (exit ${RC})" "${LOG}" ;;
esac
[[ "${RC}" -eq 0 ]]
