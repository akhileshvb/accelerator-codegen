#!/usr/bin/env bash
# Baseline: the bench target at 0% writes (and 10% for aux-state, scored on both axes),
# built and run through the engine host.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_baseline.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

JSON="${GATE_OUT}/baseline.json"
WORKLOAD_KIND="$(python3 -c "import json,sys; print(json.loads(sys.argv[1]).get('workload_kind') or '')" "${CTX}")"
export ACCEL_WORKLOAD_KIND="${WORKLOAD_KIND}"

if ! engine_ready "${BENCH_TARGET}"; then
  engine_not_ready "${JSON}" baseline "query:=${QUERY}"
fi

ALL_LOG="${ENGINE_LOG}"
FINAL_RC=0
for pct in 0 10; do
  [[ "${pct}" == 10 && "${WORKLOAD_KIND}" != "aux_state" ]] && break
  engine_measure "${BENCH_TARGET}" "${QUERY}" "${pct}"
  ALL_LOG="${ALL_LOG}"$'\n'"${LOG}"
  [[ ${RC} -eq 0 ]] || FINAL_RC=${RC}
done

# Dual-axis score for aux-state: read-only speedup floor and a write-mix cliff.
ALL_LOG="${ALL_LOG}" FINAL_RC="${FINAL_RC}" WORKLOAD_KIND="${WORKLOAD_KIND}" JSON="${JSON}" QUERY="${QUERY}" \
  MIN_RO="${ACCEL_AUX_MIN_SPEEDUP:-2.0}" python3 - <<'PY'
import json, os, pathlib, sys
log, rc, kind = os.environ["ALL_LOG"], int(os.environ["FINAL_RC"]), os.environ["WORKLOAD_KIND"]
min_ro = float(os.environ["MIN_RO"])
ro = wm = None
for line in log.splitlines():
    if line.startswith("read_only_query_speedup,"):
        ro = float(line.split(",")[1])
    if line.startswith("write_mix_workload_speedup,"):
        wm = float(line.split(",")[1])
status, reason = ("pass", "") if rc == 0 else ("fail", "bench exited non-zero")
if status == "pass" and kind == "aux_state":
    if ro is None or ro < min_ro:
        status, reason = "fail", f"read_only_query_speedup {ro} < min {min_ro} vs engine SQL"
    elif wm is not None and (ro ** 0.75) * (max(wm, 1e-9) ** 0.25) < 1.0 and wm < 0.5:
        status, reason = "fail", f"dual-axis cliff write_mix={wm}"
out = pathlib.Path(os.environ["JSON"])
out.write_text(json.dumps({
    "gate": "baseline", "query": int(os.environ["QUERY"]),
    "engine_name": os.environ["ENGINE_NAME"], "status": status, "exit_code": rc,
    "read_only_query_speedup": ro, "write_mix_workload_speedup": wm,
    "reason": reason, "log_tail": log[-8000:],
}, indent=2))
print(out.read_text())
sys.exit(0 if status == "pass" else 1)
PY
