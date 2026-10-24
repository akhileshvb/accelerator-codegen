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

# suite_adversarial_env: the adversarial phase's limits (ACCEL_ADV_*) from the suite's scenarios; the quick tier caps
# the probes at ACCEL_QUICK_PROBES, the iterate tier runs the minimum.
suite_adversarial_env() {
	local scenarios="${ACCEL_SUITE_DIR:-}/suite.yaml" exports
	if [[ -f "${scenarios}" ]]; then
		exports="$(QUICK_PROBES="${ACCEL_QUICK_PROBES:-3}" python3 - "${scenarios}" <<'PY'
import os, sys, yaml
d = yaml.safe_load(open(sys.argv[1]))
p, b = d.get("point_writes") or {}, d.get("bulk_writes") or {}
tier = os.environ.get("ACCEL_GATE_TIER")
cap = int(os.environ["QUICK_PROBES"]) if tier == "quick" else 1 << 30
max_probes = int(p.get("max_probes") or p.get("min_count") or 5)
if tier == "iterate":
    max_probes = int(p.get("min_count") or 2)
print(f"export ACCEL_ADV_MAX_PROBES={min(cap, max_probes)}")
print(f"export ACCEL_ADV_MIN_PROBES={min(cap, int(p.get('min_count') or 2))}")
print(f"export ACCEL_ADV_MIN_CHUNKS={int(b.get('min_chunks') or 0)}")
print(f"export ACCEL_ADV_REQUIRE_BULK={1 if b.get('required') else 0}")
PY
)"
		eval "${exports}"
	fi
	[[ "${ACCEL_GATE_TIER:-full}" != quick ]] || export ACCEL_ADV_MAX_PROBES="${ACCEL_ADV_MAX_PROBES:-${ACCEL_QUICK_PROBES:-3}}"
}
