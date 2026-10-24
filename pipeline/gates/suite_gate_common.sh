#!/usr/bin/env bash
# The frozen test suite in the correctness gates (suite_gate.py). Source after aux_gate_common.sh; needs ROOT.

SUITE_GATE="$(dirname "${BASH_SOURCE[0]}")/suite_gate.py"
SUITE_EXTRAS=()   # `key=value` arguments for emit_result: the run's report and how to reproduce it

# suite_prepare <tier> <result json> <gate>: export this run's ACCEL_SUITE_* (nothing when the query has no
# suite). A suite edited since the test_suite stage fails the gate before anything runs.
suite_prepare() {
	local exports err
	err="$(mktemp)"
	exports="$(python3 "${SUITE_GATE}" "${ROOT}" prepare "$1" 2>"${err}")" || emit_result "$2" "$3" fail "$(cat "${err}")" ""
	rm -f "${err}"
	eval "${exports}"
	SUITE_TIER="$1"
}

# suite_extras <gate script>: the suite's report and, with it, the command that repeats this run's draws.
suite_extras() {
	[[ -n "${ACCEL_SUITE_DIR:-}" ]] || return 0
	local summary
	summary="$(python3 "${SUITE_GATE}" "${ROOT}" summary "${ACCEL_SUITE_REPORT}")"
	SUITE_EXTRAS=("suite:=${summary}"
		"reproduce=ACCEL_GATE_CONTEXT=${ACCEL_GATE_CONTEXT} ACCEL_GATE_TIER=${SUITE_TIER} ACCEL_SUITE_SEED=${ACCEL_SUITE_SEED} bash pipeline/gates/$1 ${GATE_QUERY}")
}
