#!/usr/bin/env bash
# Validation of a freshly written test suite, the exit check of the test_suite stage: its files, then its
# tuples (the fixed ones and a fresh random draw) and transactions run on plain SQL through the engine's
# harness (ACCEL_SUITE_MODE=validate), then the checks of that report (suite_gate.py validate).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
[[ -n "${QUERY}" ]] || { echo "usage: run_test_suite_validate.sh <query_number>" >&2; exit 1; }

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
source "$(dirname "${BASH_SOURCE[0]}")/suite_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

JSON="${GATE_OUT}/test_suite_validate.json"
fail() { emit_result "${JSON}" test_suite_validate fail "$1" "${2:-}" "${SUITE_EXTRAS[@]}"; }

PROBLEMS="$(python3 "${SUITE_GATE}" "${ROOT}" validate)" || fail "the suite's files are wrong:"$'\n'"${PROBLEMS}"
suite_prepare validate "${JSON}" test_suite_validate
[[ -n "${ACCEL_SUITE_DIR:-}" ]] || fail "no suite directory"
[[ -n "${DIFF_BINARY}" ]] || fail "no differential target in the gate context. ${HOST_HINT}"
engine_ready "${DIFF_BINARY}" || fail "${ENGINE_FAIL}" "${ENGINE_LOG}"
engine_run "${DIFF_BINARY}" "${QUERY}"
suite_extras run_test_suite_validate.sh
[[ -s "${ACCEL_SUITE_REPORT}" ]] || fail "the suite does not run on plain SQL (exit ${RC})" "${ENGINE_LOG}${LOG}"
PROBLEMS="$(python3 "${SUITE_GATE}" "${ROOT}" validate "${ACCEL_SUITE_REPORT}")" ||
	fail "the suite does not pass validation:"$'\n'"${PROBLEMS}" "${ENGINE_LOG}${LOG}"
emit_result "${JSON}" test_suite_validate pass "" "${ENGINE_LOG}${LOG}" "${SUITE_EXTRAS[@]}"
