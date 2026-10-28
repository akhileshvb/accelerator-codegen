#!/usr/bin/env bash
# Backend-generic slot gates (engine_backend validation). One script serves every
# slot_* gate: the gate name picks the scenario, the engine's host.sh runs it.
#
#   slot_epoch_stress        writers + readers at once, exact at each reader's snapshot
#   slot_hits_under_writes   continuous commits, exact answers, minimum hit rate
#                            (every isolation level the engine has)
#   slot_read_after_commit   another connection reading right after a commit sees it
#   slot_txn_edges           aborts, savepoints, TRUNCATE
#   slot_restart             accelerator workers / processes restarted under load
#   slot_own_writes          reads inside a writing transaction include its own rows
#   slot_ddl_events          replacing / renaming a tracked table never leaves a stale answer
#
# `host.sh slot <scenario> <seconds>` exits 0 (pass), 1 (fail) or 77 (the engine profile
# declares the mechanic n/a: gate skipped; never for own_writes or ddl_events); see pipeline/prompts/engine_backend/host_contract.md.
# Env: ACCEL_SLOT_GATE_SECONDS (load duration, default 6), ACCEL_SLOT_GATE_TIMEOUT
# (seconds before a hung backend fails the gate; default 900).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
GATE="${ACCEL_GATE_NAME:-slot_epoch_stress}"
OUT_JSON="${ACCEL_GATE_OUT:-/tmp/${GATE}.json}"
SCENARIO="${GATE#slot_}"
SECONDS_PER_SCENARIO="${ACCEL_SLOT_GATE_SECONDS:-6}"
DEFAULT_TIMEOUT=900
TIMEOUT_SECONDS="${ACCEL_SLOT_GATE_TIMEOUT:-${DEFAULT_TIMEOUT}}"

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
if [[ -n "${ACCEL_GATE_CONTEXT:-}" && -f "${ACCEL_GATE_CONTEXT}" ]]; then
  load_engine_gate_context "${ROOT}" 0
else
  ENGINE_NAME="${ACCEL_ENGINE_NAME:-}"
  [[ -n "${ENGINE_NAME}" ]] && setup_engine_host "${ROOT}"
fi
ENGINE="${ENGINE_NAME}"

mkdir -p "$(dirname "${OUT_JSON}")"
if [[ -z "${ENGINE}" || ! -x "${ENGINE_HOST}" ]]; then
  write_result "${OUT_JSON}" "${GATE}" fail "no host.sh for engine '${ENGINE}'" "" || exit 1
fi

# A hung backend (a reader waiting forever for a commit that never publishes)
# must fail the gate, not the pipeline.
LOG="$(timeout -k 10 "${TIMEOUT_SECONDS}" "${ENGINE_HOST}" slot "${SCENARIO}" "${SECONDS_PER_SCENARIO}" 2>&1)"
RC=$?
echo "${LOG}"
case "${RC}" in
  0)  write_result "${OUT_JSON}" "${GATE}" pass "${SCENARIO}: ${ENGINE} passed" "${LOG}" >/dev/null ;;
  77) write_result "${OUT_JSON}" "${GATE}" skip "${SCENARIO}: not applicable to ${ENGINE}" "${LOG}" >/dev/null; exit 0 ;;
  124) write_result "${OUT_JSON}" "${GATE}" fail "${SCENARIO}: ${ENGINE} hung (no result in ${TIMEOUT_SECONDS}s)" "${LOG}" >/dev/null ;;
  *)  write_result "${OUT_JSON}" "${GATE}" fail "${SCENARIO}: ${ENGINE} failed (exit ${RC})" "${LOG}" >/dev/null ;;
esac
[[ "${RC}" -eq 0 ]]
