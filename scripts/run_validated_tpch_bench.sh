#!/usr/bin/env bash
# Clean, reproducible validated TPC-H benchmark for all implemented accelerators:
# Q1, Q3, Q4, Q6, Q9, Q12, Q14.
#
# Usage:
#   ./scripts/run_validated_tpch_bench.sh                  # SF1, session 1
#   ./scripts/run_validated_tpch_bench.sh --scale-factor 10
#   ./scripts/run_validated_tpch_bench.sh --scale-factor 10 --session 2 --seeds 1-5
#   ./scripts/run_validated_tpch_bench.sh 2                # legacy: session 2, SF1
#
# Production kernel paths are hardcoded (no ambient ACCEL_* required).
# This script clears experimental ACCEL_* / DUCKDB_* vars so shell inheritance cannot skew results.
set -euo pipefail

ROOT=/home/akhilvb/accelerator-codegen
DUCKDB_ROOT=/home/akhilvb/accelerator-codegen-duckdb
BUILD="${ROOT}/build-release"
OUT_DIR="${ROOT}/benchmarks/tpch/results"

SCALE=1
SESSION=1
SEEDS=""  # empty = all (SF1 default); SF10 defaults to 1-5 unless overridden

while [[ $# -gt 0 ]]; do
  case "$1" in
    --scale-factor)
      SCALE="${2:?}"
      shift 2
      ;;
    --session)
      SESSION="${2:?}"
      shift 2
      ;;
    --seeds)
      SEEDS="${2:?}"
      shift 2
      ;;
    [0-9]*)
      SESSION="$1"
      shift
      ;;
    -h|--help)
      sed -n '2,12p' "$0"
      exit 0
      ;;
    *)
      echo "unknown arg: $1" >&2
      exit 1
      ;;
  esac
done

if [[ "${SCALE}" != "1" && "${SCALE}" != "10" ]]; then
  echo "ERROR: --scale-factor must be 1 or 10 (got ${SCALE})" >&2
  exit 1
fi

SF_DB="${DUCKDB_ROOT}/tpch_sf${SCALE}.duckdb"
if [[ ! -f "${SF_DB}" ]]; then
  echo "ERROR: missing database ${SF_DB}" >&2
  exit 1
fi

# SF10 default seed window for practical runtime; SF1 keeps full manifests.
if [[ -z "${SEEDS}" ]]; then
  if [[ "${SCALE}" == "10" ]]; then
    SEEDS="1-5"
  else
    SEEDS="all"
  fi
fi

seed_filter() {
  local q="$1"
  local parts=()
  local i
  if [[ "${SEEDS}" == "all" ]]; then
    echo ""
    return
  fi
  if [[ "${SEEDS}" =~ ^([0-9]+)-([0-9]+)$ ]]; then
    local lo="${BASH_REMATCH[1]}"
    local hi="${BASH_REMATCH[2]}"
    for ((i = lo; i <= hi; i++)); do
      parts+=("seed_${i}_q${q}")
    done
  else
    IFS=',' read -r -a nums <<< "${SEEDS}"
    for i in "${nums[@]}"; do
      parts+=("seed_${i}_q${q}")
    done
  fi
  local IFS=,
  # Validated binary uses one global filter across all queries; include every qN label.
  echo "${parts[*]}"
}

build_global_filter() {
  if [[ "${SEEDS}" == "all" ]]; then
    echo ""
    return
  fi
  local all=()
  local q
  for q in 1 3 4 6 9 12 14; do
    local f
    f="$(seed_filter "${q}")"
    IFS=',' read -r -a chunk <<< "${f}"
    all+=("${chunk[@]}")
  done
  local IFS=,
  echo "${all[*]}"
}

preflight_sf10() {
  local free_gb mem_gb
  free_gb=$(df -BG "${DUCKDB_ROOT}" | awk 'NR==2 {gsub(/G/,"",$4); print $4}')
  mem_gb=$(awk '/MemAvailable/ {printf "%d", $2/1024/1024}' /proc/meminfo)
  echo "preflight_free_disk_gb=${free_gb} preflight_mem_available_gb=${mem_gb}"
  if [[ "${free_gb}" -lt 20 ]]; then
    echo "ERROR: need >= 20 GB free disk" >&2
    exit 1
  fi
  if [[ "${mem_gb}" -lt 16 ]]; then
    echo "ERROR: need >= 16 GB MemAvailable" >&2
    exit 1
  fi
}

mkdir -p "${OUT_DIR}"

echo "=== build identity ==="
echo "accelerator_commit=$(git -C "${ROOT}" rev-parse HEAD)"
echo "duckdb_commit=$(git -C "${DUCKDB_ROOT}" rev-parse HEAD)"
echo "compiler=$(c++ --version | head -1)"
echo "scale_factor=${SCALE}"
echo "session=${SESSION}"
echo "seeds=${SEEDS}"
echo "db=${SF_DB}"

# Drop any inherited experimental toggles that could affect DuckDB-side paths.
while IFS= read -r name; do
  unset "${name}" || true
done < <(env | awk -F= '/^ACCEL_Q|^DUCKDB_BITPACKING|^DUCKDB_Q6_/ {print $1}')

export ACCEL_TPCH_DB="${SF_DB}"
export ACCEL_TPCH_SCALE="${SCALE}"
export ACCEL_BENCH_SESSION="${SESSION}"
export DUCKDB_BITPACKING_NATIVE_FILTER=0
export ACCEL_Q1_AUX_STATE=0

if [[ "${SCALE}" == "10" ]]; then
  preflight_sf10
  export ACCEL_BENCH_WARMUP=3
  export ACCEL_BENCH_ITERS=5
else
  export ACCEL_BENCH_WARMUP=3
  export ACCEL_BENCH_ITERS=10
fi

FILTER="$(build_global_filter)"
if [[ -n "${FILTER}" ]]; then
  export ACCEL_BENCH_FILTER="${FILTER}"
  echo "ACCEL_BENCH_FILTER=${ACCEL_BENCH_FILTER}"
else
  unset ACCEL_BENCH_FILTER || true
fi

cmake --build "${BUILD}" -j "$(nproc)" --target benchmark_tpch_validated

CSV="${OUT_DIR}/validated_sf${SCALE}_session${SESSION}.csv"
LOG="${OUT_DIR}/validated_sf${SCALE}_session${SESSION}.log"
# Keep legacy SF1 session1 alias for existing tooling.
if [[ "${SCALE}" == "1" && "${SESSION}" == "1" ]]; then
  ln -sfn "validated_sf1_session1.csv" "${OUT_DIR}/validated_tpch_all_queries.csv" 2>/dev/null || true
fi

echo "=== running SF${SCALE} session ${SESSION} -> ${CSV} ==="
env -u ACCEL_Q1_FUSED_SCAN -u ACCEL_Q1_FUSED_ALIGNED_PAYLOAD -u ACCEL_Q3_DENSE_CUST \
  -u ACCEL_Q4_LINEITEM_FUSED -u ACCEL_Q9_DENSE_ORDERS -u ACCEL_Q12_DENSE_ORDERS -u ACCEL_Q14_DENSE_PART \
  "${BUILD}/benchmark_tpch_validated" >"${CSV}" 2>"${LOG}"
echo "=== session ${SESSION} complete ==="
tail -n 60 "${LOG}"
