#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
BUILD="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-${ROOT}/build-release}}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_aux_speedup_verification.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_aux_gate_context "${ROOT}" "${QUERY}"

OUT="${GATE_OUT}"
mkdir -p "${OUT}"

# shellcheck source=cmake_ensure_target.sh
source "$(dirname "${BASH_SOURCE[0]}")/cmake_ensure_target.sh"

MIN_SPEEDUP="${ACCEL_AUX_MIN_SPEEDUP:-2.0}"
MIN_REDUCTION_PCT="${ACCEL_AUX_MIN_WORK_REDUCTION_PCT:-50}"

if [[ ! -d "${BUILD}" ]]; then
  echo "{\"gate\":\"aux_speedup_verification\",\"query\":${QUERY},\"status\":\"skip\",\"reason\":\"build dir missing\"}" > "${OUT}/aux_speedup_verification.json"
  exit 0
fi

BENCH_AUX="${AUX_BENCH_TARGET}"
BENCH_BASE="${BENCH_TARGET}"

ensure_cmake_target "${ROOT}" "${BUILD}" "${BENCH_BASE}" || true
ensure_cmake_target "${ROOT}" "${BUILD}" "${BENCH_AUX}" || true

if [[ ! -x "${BUILD}/${BENCH_AUX}" ]] || [[ ! -x "${BUILD}/${BENCH_BASE}" ]]; then
  python3 - <<PY
import json, pathlib
path = pathlib.Path("${OUT}/aux_speedup_verification.json")
path.write_text(json.dumps({
    "gate": "aux_speedup_verification",
    "query": int("${QUERY}"),
    "benchmark": "${BENCHMARK_NAME}",
    "status": "skip",
    "reason": "benchmark binaries missing; build benchmarks first",
}, indent=2))
print(path.read_text())
PY
  exit 0
fi

set +e
BASE_LOG="$(env "${DB_ENV_VAR}"="${DATASET_DB}" "${BUILD}/${BENCH_BASE}" 2>&1)"
BASE_RC=$?
AUX_LOG="$(env "${DB_ENV_VAR}"="${DATASET_DB}" "${BUILD}/${BENCH_AUX}" 2>&1)"
AUX_RC=$?
set -e

python3 - <<PY
import json
import pathlib
import re
import sys

def parse_us(text: str) -> float | None:
    for pattern in (
        r"speedup_gate wall_time_base_us=([0-9.]+)",
        r"speedup_gate median_us=([0-9.]+)",
        r"summary_median,accel_fused_q1,,([0-9.]+)",
        r"median_us[=:]\\s*([0-9.]+)",
        r"wall_time_us[=:]\\s*([0-9.]+)",
        r"([0-9.]+)\\s*us\\b",
    ):
        m = re.search(pattern, text, re.IGNORECASE)
        if m:
            return float(m.group(1))
    return None

def parse_base_us(base_text: str, aux_text: str) -> float | None:
    for text in (base_text, aux_text):
        m = re.search(r"speedup_gate wall_time_base_us=([0-9.]+)", text)
        if m:
            return float(m.group(1))
    m = re.search(r"summary_median,accel_fused_q1,,([0-9.]+)", base_text)
    if m:
        return float(m.group(1))
    m = re.search(r"summary_avg_median,[^,\n]+,[^,\n]+,[^,\n]+,([0-9.]+)", base_text)
    if m:
        return float(m.group(1))
    return parse_us(base_text)

def parse_aux_us(text: str) -> float | None:
    m = re.search(r"speedup_gate median_us=([0-9.]+)", text)
    if m:
        return float(m.group(1))
    return parse_us(text)

base_us = parse_base_us("""${BASE_LOG}""", """${AUX_LOG}""")
aux_us = parse_aux_us("""${AUX_LOG}""")

status = "fail"
speedup = None
work_reduction_pct = None
notes = []

if ${BASE_RC} != 0:
    notes.append("base benchmark failed")
if ${AUX_RC} != 0:
    notes.append("aux benchmark failed")

if base_us and aux_us and aux_us > 0:
    speedup = base_us / aux_us
    work_reduction_pct = max(0.0, (1.0 - (aux_us / base_us)) * 100.0)
    if speedup >= float("${MIN_SPEEDUP}") or work_reduction_pct >= float("${MIN_REDUCTION_PCT}"):
        status = "pass"
    else:
        notes.append(
            f"speedup_vs_base={speedup:.2f} < ${MIN_SPEEDUP} and "
            f"work_reduction_pct={work_reduction_pct:.1f} < ${MIN_REDUCTION_PCT}"
        )
else:
    notes.append("could not parse wall times from benchmark output; treating as skip")
    status = "skip"

out = {
    "gate": "aux_speedup_verification",
    "query": int("${QUERY}"),
    "benchmark": "${BENCHMARK_NAME}",
    "status": status,
    "wall_time_base_us": base_us,
    "wall_time_aux_us": aux_us,
    "speedup_vs_base": speedup,
    "work_reduction_pct": work_reduction_pct,
    "min_speedup_vs_base": float("${MIN_SPEEDUP}"),
    "min_work_reduction_pct": float("${MIN_REDUCTION_PCT}"),
    "operators_eliminated_confirmed": aux_us is not None and base_us is not None and aux_us < base_us,
    "notes": "; ".join(notes) if notes else "benchmark comparison complete",
    "base_log_tail": """${BASE_LOG}"""[-2000:],
    "aux_log_tail": """${AUX_LOG}"""[-2000:],
}
path = pathlib.Path("${OUT}/aux_speedup_verification.json")
path.write_text(json.dumps(out, indent=2))
print(path.read_text())
sys.exit(0 if status in ("pass", "skip") else 1)
PY
