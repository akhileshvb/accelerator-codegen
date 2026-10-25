#!/usr/bin/env bash
# The stage-exit run of an aux accelerator: the heavy checks side by side instead of one gate after another.
# The targets are built once, then these phases run at the same time, each the gate script that also runs alone, so
# its result file is the one that gate writes (aux_correctness.json, aux_bench_sweep.json, ...):
#   correctness       smoke test; the differential at N threads and the suite at one thread (one module build each)
#   multi_connection  readers and a writer on separate connections
#   cross_accel       the accelerator beside the others
#   tsan              the differential on the subset dataset under ThreadSanitizer (its own build dir)
#   bench             the sweep of every write frequency in one process, on the measurement CPUs, on an instance of its
#                     own (the bench writes the database)
# The cost-model calibration is a gate of its own after this one: it rebuilds the state several times.
# aux_exit.json: the status, seconds and result file of each phase, and the first failure's reason.
# ACCEL_EXIT_PHASES lists the phases to run (default: all).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_exit.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

JSON="${GATE_OUT}/${ACCEL_GATE_NAME:-aux_exit}.json"
GATES="$(dirname "${BASH_SOURCE[0]}")"
read -r -a PHASES <<<"${ACCEL_EXIT_PHASES:-correctness multi_connection cross_accel tsan bench}"

declare -A GATE=([correctness]=aux_correctness [multi_connection]=aux_multi_connection [cross_accel]=aux_cross_accel
  [tsan]=aux_tsan [bench]=aux_bench_sweep)
[[ "${ACCEL_GATE_CHANGE_KIND:-}" != optimization ]] || GATE[correctness]=aux_correctness_iterate   # the lighter tier
declare -A SCRIPT=([correctness]=run_aux_correctness.sh [multi_connection]=run_aux_multi_connection.sh
  [cross_accel]=run_aux_cross_accel.sh [tsan]=run_aux_tsan.sh [bench]=run_aux_bench_sweep.sh)

mapfile -t TARGETS < <(printf '%s\n' "${AUX_TEST_TARGET}" "${DIFF_BINARY}" "${MULTI_CONN_TARGET}" "${CROSS_ACCEL_TARGET}" \
  "${AUX_BENCH_TARGET}" | grep .)
if ! engine_ready "${TARGETS[@]}"; then
  emit_result "${JSON}" aux_exit fail "${ENGINE_FAIL}" "${ENGINE_LOG}"
fi

# start <phase>: the phase's gate script in the background, as that gate (its result file is the gate's own); status,
# seconds and output in <gates dir>/aux_exit/<phase>.*. The bench writes the database: it runs on an instance of its
# own and queues for the measurement lease. The TSAN run is on the subset dataset.
RUNS="${GATE_OUT}/aux_exit"
rm -rf "${RUNS}"
mkdir -p "${RUNS}"
declare -A RUNNING
start() {
  local name="$1" own=()
  case "${name}" in
    bench) own=("ACCEL_ENGINE_INSTANCE=${ACCEL_ENGINE_INSTANCE}-${name}" "ACCEL_ENGINE_STATE_DIR=${ACCEL_ENGINE_STATE_DIR}-${name}") ;;
    tsan) own=(ACCEL_GATE_TIER=quick) ;;
    correctness) [[ "${GATE[correctness]}" != aux_correctness_iterate ]] || own=(ACCEL_GATE_TIER=iterate) ;;
  esac
  rm -f "${GATE_OUT}/${GATE[${name}]}.json"
  (
    started="${SECONDS}"
    env "ACCEL_GATE_NAME=${GATE[${name}]}" "ACCEL_GATE_OUT=${GATE_OUT}/${GATE[${name}]}.json" "${own[@]}" \
      bash "${GATES}/${SCRIPT[${name}]}" "${QUERY}" >"${RUNS}/${name}.log" 2>&1
    echo $? >"${RUNS}/${name}.rc"
    echo $((SECONDS - started)) >"${RUNS}/${name}.seconds"
  ) &
  RUNNING[${name}]=$!
}

for name in "${PHASES[@]}"; do
  [[ -n "${GATE[${name}]:-}" ]] || emit_result "${JSON}" aux_exit fail "unknown phase ${name}" ""
  start "${name}"
done
set_phase "running: ${PHASES[*]}"
for name in "${PHASES[@]}"; do
  wait "${RUNNING[${name}]}"
done

PHASE_GATES=()
for name in "${PHASES[@]}"; do PHASE_GATES+=("${name}=${GATE[${name}]}"); done
python3 - "${JSON}" "${RUNS}" "${GATE_OUT}" "${PHASE_GATES[@]}" <<'PY'
import json, pathlib, sys

out, runs, gates, *phases = sys.argv[1:]
runs, gates = pathlib.Path(runs), pathlib.Path(gates)
report, failed = {}, []
for phase in phases:
    name, _, gate = phase.partition("=")
    result = gates / f"{gate}.json"
    data = json.loads(result.read_text()) if result.is_file() else {}
    rc = int((runs / f"{name}.rc").read_text())
    status = data.get("status") if rc == 0 else "fail"
    report[name] = {"status": status or "pass", "seconds": float((runs / f"{name}.seconds").read_text()),
                    "result": str(result) if result.is_file() else ""}
    if report[name]["status"] not in ("pass", "skip"):
        failed.append((name, data, (runs / f"{name}.log").read_text()[-4000:]))
first = failed[0] if failed else None
payload = {
    "gate": "aux_exit", "status": "fail" if failed else "pass",
    "reason": f"{first[0]}: {first[1].get('reason') or 'failed'}" if first else "",
    "failed_phases": [name for name, *_ in failed], "phases": report,
    "log_tail": (first[1].get("log_tail") or first[2]) if first else "",
}
pathlib.Path(out).write_text(json.dumps(payload, indent=2))
print(json.dumps(payload, indent=2))
sys.exit(1 if failed else 0)
PY
