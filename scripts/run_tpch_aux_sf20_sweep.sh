#!/usr/bin/env bash
# TPC-H SF20 aux-state wall-clock write-interval sweep for all benchmarked aux kernels.
#
# Measures workload_speedup and query_only_speedup at write intervals (seconds).
# Results land under build-release/q{N}_aux_bench_sf20/results.csv per query.
#
# Usage:
#   bash scripts/setup_tpch_sf20_db.sh          # once, if DB missing (~50 GB disk)
#   bash scripts/run_tpch_aux_sf20_sweep.sh     # full sweep (slow; hours)
#   bash scripts/run_tpch_aux_sf20_sweep.sh smoke   # fast sanity check (Q11 by default)
#   QUERIES=11 bash scripts/run_tpch_aux_sf20_sweep.sh
#
# Outputs:
#   build-release/q{N}_aux_bench_sf20/results.csv
#   build-release/tpch_aux_sf20_sweep_summary.csv
#   benchmarks/tpch/results/tpch_aux_sf20_sweep.log
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DUCKDB_ROOT="${DUCKDB_ROOT:-${ROOT}/../accelerator-codegen-duckdb}"
BUILD="${BUILD:-${ROOT}/build-release}"
CONFIG="${CONFIG:-${ROOT}/pipeline/config/benchmark_packs/tpch_sf20_aux_sweep.yaml}"
OUT_DIR="${OUT_DIR:-${ROOT}/benchmarks/tpch/results}"
LOG="${LOG:-${OUT_DIR}/tpch_aux_sf20_sweep.log}"

MODE="${1:-full}" # smoke | full

read_yaml_list() {
  local key="$1"
  local nested="${2:-}"
  python3 - <<PY
import pathlib
try:
    import yaml
except ImportError:
    yaml = None
cfg_path = pathlib.Path("${CONFIG}")
values = []
if yaml and cfg_path.is_file():
    cfg = yaml.safe_load(cfg_path.read_text()) or {}
    if "${nested}":
        cfg = cfg.get("${nested}", {}) or {}
    values = cfg.get("${key}", []) or []
print(",".join(str(v) for v in values))
PY
}

read_yaml_scalar() {
  local section="$1"
  local key="$2"
  local fallback="$3"
  python3 - <<PY
import pathlib
try:
    import yaml
except ImportError:
    yaml = None
cfg_path = pathlib.Path("${CONFIG}")
value = "${fallback}"
if yaml and cfg_path.is_file():
    cfg = yaml.safe_load(cfg_path.read_text()) or {}
    section = cfg.get("${section}", {}) or {}
    if "${key}" in section:
        value = section["${key}"]
print(value)
PY
}

read_yaml_top_scalar() {
  local key="$1"
  local fallback="$2"
  python3 - <<PY
import pathlib
try:
    import yaml
except ImportError:
    yaml = None
cfg_path = pathlib.Path("${CONFIG}")
value = "${fallback}"
if yaml and cfg_path.is_file():
    cfg = yaml.safe_load(cfg_path.read_text()) or {}
    if "${key}" in cfg:
        value = cfg["${key}"]
print(value)
PY
}

SF20_DB="${ACCEL_TPCH_DB:-${DUCKDB_ROOT}/tpch_sf20.duckdb}"
DB_LABEL="${ACCEL_AUX_BENCH_DB_LABEL:-tpch_sf20}"
DEFAULT_QUERIES="$(read_yaml_list queries)"
SMOKE_QUERIES="$(read_yaml_list queries smoke)"
DEFAULT_INTERVALS="$(read_yaml_list write_intervals)"
SMOKE_INTERVALS="$(read_yaml_list write_intervals smoke)"
DEFAULT_DURATION="$(read_yaml_top_scalar bench_duration_sec 120)"
SMOKE_DURATION="$(read_yaml_scalar smoke bench_duration_sec "${DEFAULT_DURATION}")"
DEFAULT_MIN_WRITES="$(read_yaml_top_scalar bench_min_writes 5)"
SMOKE_MIN_WRITES="$(read_yaml_scalar smoke bench_min_writes "${DEFAULT_MIN_WRITES}")"
MIN_DISK_GB="$(read_yaml_scalar preflight min_disk_gb 50)"
MIN_MEM_GB="$(read_yaml_scalar preflight min_mem_gb 24)"

if [[ "${MODE}" == "smoke" ]]; then
  QUERIES="${QUERIES:-${SMOKE_QUERIES}}"
  [[ -z "${QUERIES}" ]] && QUERIES="11"
else
  QUERIES="${QUERIES:-${DEFAULT_QUERIES}}"
fi

preflight() {
  local free_gb mem_gb
  free_gb=$(df -BG "$(dirname "${SF20_DB}")" | awk 'NR==2 {gsub(/G/,"",$4); print $4}')
  mem_gb=$(awk '/MemAvailable/ {printf "%d", $2/1024/1024}' /proc/meminfo)
  echo "preflight_free_disk_gb=${free_gb} preflight_mem_available_gb=${mem_gb}"
  if [[ "${free_gb}" -lt "${MIN_DISK_GB}" ]]; then
    echo "ERROR: need >= ${MIN_DISK_GB} GB free disk, have ${free_gb} GB" >&2
    exit 1
  fi
  if [[ "${mem_gb}" -lt "${MIN_MEM_GB}" ]]; then
    echo "ERROR: need >= ${MIN_MEM_GB} GB MemAvailable, have ${mem_gb} GB" >&2
    exit 1
  fi
  if [[ ! -f "${SF20_DB}" ]]; then
    echo "ERROR: missing ${SF20_DB}" >&2
    echo "run: bash scripts/setup_tpch_sf20_db.sh" >&2
    exit 1
  fi
}

apply_mode_env() {
  local mode="$1"
  unset ACCEL_BENCH_FAST ACCEL_BENCH_SKIP_DUCKDB_REF ACCEL_Q21_BENCH_SCAN_LIMIT || true
  # Interval cadence is the SF20 sweep axis. Pin frequency loop to a single
  # pass so run_aux_bench_sweep.sh does not re-run the full interval grid.
  export ACCEL_AUX_WRITE_FREQUENCIES="${ACCEL_AUX_WRITE_FREQUENCIES:-0}"
  if [[ "${mode}" == "smoke" ]]; then
    export ACCEL_BENCH_FAST=1
    export ACCEL_AUX_WRITE_INTERVALS="${ACCEL_AUX_WRITE_INTERVALS:-${SMOKE_INTERVALS:-0,10}}"
    export ACCEL_AUX_BENCH_DURATION_SEC="${ACCEL_AUX_BENCH_DURATION_SEC:-${SMOKE_DURATION}}"
    export ACCEL_AUX_BENCH_MIN_WRITES="${ACCEL_AUX_BENCH_MIN_WRITES:-${SMOKE_MIN_WRITES}}"
    export ACCEL_BENCH_WARMUP="${ACCEL_BENCH_WARMUP:-1}"
    export ACCEL_BENCH_REF_SAMPLES="${ACCEL_BENCH_REF_SAMPLES:-0}"
    export ACCEL_BENCH_BASE_WRITE_SAMPLES="${ACCEL_BENCH_BASE_WRITE_SAMPLES:-0}"
  else
    export ACCEL_AUX_WRITE_INTERVALS="${ACCEL_AUX_WRITE_INTERVALS:-${DEFAULT_INTERVALS:-0,60,30,10,5,2,1}}"
    export ACCEL_AUX_BENCH_DURATION_SEC="${ACCEL_AUX_BENCH_DURATION_SEC:-${DEFAULT_DURATION}}"
    export ACCEL_AUX_BENCH_MIN_WRITES="${ACCEL_AUX_BENCH_MIN_WRITES:-${DEFAULT_MIN_WRITES}}"
    export ACCEL_BENCH_WARMUP="${ACCEL_BENCH_WARMUP:-10}"
    export ACCEL_BENCH_REF_SAMPLES="${ACCEL_BENCH_REF_SAMPLES:-3}"
    export ACCEL_BENCH_BASE_WRITE_SAMPLES="${ACCEL_BENCH_BASE_WRITE_SAMPLES:-3}"
  fi
}

build_targets() {
  local targets=()
  IFS=',' read -r -a query_list <<< "${QUERIES}"
  for query in "${query_list[@]}"; do
    targets+=("benchmark_tpch_q${query}_aux")
  done
  echo "=== building ${#targets[@]} aux benchmark targets ==="
  cmake --build "${BUILD}" -j "$(nproc)" --target "${targets[@]}"
}

run_query_sweep() {
  local query="$1"
  local log="${OUT_DIR}/tpch_aux_sf20_q${query}.log"
  echo "=== SF20 aux sweep Q${query} ===" | tee -a "${LOG}"
  (
    export ACCEL_BUILD_DIR="${BUILD}"
    export ACCEL_TPCH_DB="${SF20_DB}"
    export ACCEL_AUX_BENCH_DB_LABEL="${DB_LABEL}"
    # Work DB copies are large (~SF size). Prefer ACCEL_AUX_BENCH_WORK_ROOT on
    # a big disk (e.g. /ssd1/akhilvb/aux_bench); default remains under BUILD.
    export ACCEL_AUX_BENCH_WORK_DIR="${ACCEL_AUX_BENCH_WORK_ROOT:-${BUILD}}/q${query}_aux_bench_${DB_LABEL}"
    export ACCEL_AUX_BENCH_BASELINE_CACHE="${ROOT}/config/q${query}_aux_benchmark_baseline_sf20.json"
    export ACCEL_REPO_ROOT="${ROOT}"
    export ACCEL_BENCH_SKIP_PARTITION_REBUILD_CALIB=1
    bash "${ROOT}/scripts/run_aux_bench_sweep.sh" "${query}"
  ) 2>&1 | tee "${log}" | tee -a "${LOG}"
}

mkdir -p "${OUT_DIR}"
: >"${LOG}"

echo "=== TPC-H SF20 aux write-interval sweep ===" | tee -a "${LOG}"
echo "sf20_db=${SF20_DB}" | tee -a "${LOG}"
echo "queries=${QUERIES}" | tee -a "${LOG}"
echo "mode=${MODE}" | tee -a "${LOG}"

preflight
apply_mode_env "${MODE}"
echo "write_intervals_sec=${ACCEL_AUX_WRITE_INTERVALS}" | tee -a "${LOG}"
echo "write_frequencies_pct=${ACCEL_AUX_WRITE_FREQUENCIES}" | tee -a "${LOG}"
echo "bench_duration_sec=${ACCEL_AUX_BENCH_DURATION_SEC}" | tee -a "${LOG}"
build_targets

IFS=',' read -r -a query_list <<< "${QUERIES}"
for query in "${query_list[@]}"; do
  run_query_sweep "${query}"
done

python3 "${ROOT}/scripts/summarize_tpch_aux_sf20_sweep.py" \
  --build-dir "${BUILD}" \
  --label "${DB_LABEL}" \
  --queries "${QUERIES}" \
  --config "${CONFIG}"

echo "=== complete; log=${LOG} ==="
