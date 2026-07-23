#!/usr/bin/env bash
# Parallel accelerator matrix for all 22 TPC-H queries (matched-thread DuckDB).
#
# Usage:
#   ./scripts/run_parallel_22q_matrix.sh --scale-factor 1
#   ./scripts/run_parallel_22q_matrix.sh --scale-factor 10
#   ./scripts/run_parallel_22q_matrix.sh --scale-factor 10 --threads 1,2,4,8
#
# Produces benchmarks/tpch/results/sf{N}_parallel_22q/geomean_by_query.csv and plot PNG.
set -euo pipefail

ROOT=/home/akhilvb/accelerator-codegen
DUCKDB_ROOT=/home/akhilvb/accelerator-codegen-duckdb
BUILD="${ROOT}/build-release-parallel"
SCALE=1
THREAD_LIST="1,2,4,8,16"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --scale-factor) SCALE="${2:?}"; shift 2 ;;
    --threads) THREAD_LIST="${2:?}"; shift 2 ;;
    -h|--help)
      sed -n '2,9p' "$0"
      exit 0
      ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac
done

if [[ "${SCALE}" != "1" && "${SCALE}" != "10" ]]; then
  echo "ERROR: --scale-factor must be 1 or 10 (got ${SCALE})" >&2
  exit 1
fi

DB="${DUCKDB_ROOT}/tpch_sf${SCALE}.duckdb"
OUT="${ROOT}/benchmarks/tpch/results/sf${SCALE}_parallel_22q"
LOG="${OUT}/campaign.log"
CSV="${OUT}/geomean_by_query.csv"
PLOT_SCRIPT="${ROOT}/benchmarks/tpch/plot_sf${SCALE}_parallel_speedup.py"

IFS=',' read -r -a THREADS <<< "${THREAD_LIST}"

mkdir -p "${OUT}"
if [[ -f "${CSV}" ]] && [[ $(wc -l <"${CSV}") -gt 1 ]]; then
  log() { echo "[$(date -Is)] $*" | tee -a "${LOG}"; }
  log "Resuming from existing ${CSV}"
else
  echo "query,threads,geomean_speedup,source,log_file" >"${CSV}"
  log() { echo "[$(date -Is)] $*" | tee -a "${LOG}"; }
fi

already_done() {
  local query=$1 threads=$2
  awk -F, -v q="${query}" -v t="${threads}" 'NR>1 && $1==q && $2==t {found=1} END{exit !found}' "${CSV}"
}

append_geomean() {
  local query=$1 threads=$2 geomean=$3 source=$4 logfile=$5
  if already_done "${query}" "${threads}"; then
    log "SKIP ${query} t=${threads} (already recorded)"
    return 0
  fi
  echo "${query},${threads},${geomean},${source},${logfile}" >>"${CSV}"
}

parse_geomean_from_log() {
  local query=$1
  awk -F, -v q="${query}" '
    $1=="summary" && $2==q && $3=="geomean" {print $NF; exit}
    $1=="summary_geomean_speedup" {print $NF; exit}
  '
}

parse_validated_geomean() {
  local query=$1
  grep -aE "^${query}: seeds=" | sed -n 's/.*geomean_speedup=\([0-9.]*\).*/\1/p' | head -1
}

run_individual() {
  local query=$1 bin=$2 threads=$3 extra_env=${4:-}
  if already_done "${query}" "${threads}"; then
    log "SKIP ${query} t=${threads} (already recorded)"
    return 0
  fi
  local logfile="${OUT}/${query}_t${threads}.log"
  log "RUN ${query} t=${threads} (${bin})"
  # shellcheck disable=SC2086
  env ACCEL_BENCH_THREADS="${threads}" ${extra_env} "${BUILD}/${bin}" >"${logfile}" 2>&1
  local geomean
  geomean=$(parse_geomean_from_log "${query}" <"${logfile}" || true)
  if [[ -z "${geomean}" ]]; then
    log "ERROR: no geomean for ${query} t=${threads}; see ${logfile}"
    return 1
  fi
  append_geomean "${query}" "${threads}" "${geomean}" "${bin}" "${logfile}"
  log "OK ${query} t=${threads} geomean=${geomean}"
}

run_validated_subset() {
  local threads=$1
  local missing=0
  for q in Q4 Q6 Q9 Q12 Q14; do
    if ! already_done "${q}" "${threads}"; then
      missing=1
      break
    fi
  done
  if [[ "${missing}" -eq 0 ]]; then
    log "SKIP validated t=${threads} (already recorded)"
    return 0
  fi
  local logfile="${OUT}/validated_t${threads}.log"
  log "RUN validated (Q4,Q6,Q9,Q12,Q14) t=${threads}"
  env ACCEL_BENCH_THREADS="${threads}" \
    "${BUILD}/benchmark_tpch_validated" --accelerator-mode parallel --threads "${threads}" \
    >"${logfile}" 2>&1
  for q in Q4 Q6 Q9 Q12 Q14; do
    local geomean
    geomean=$(parse_validated_geomean "${q}" <"${logfile}")
    if [[ -z "${geomean}" ]]; then
      log "ERROR: validated missing ${q} t=${threads}"
      return 1
    fi
    append_geomean "${q}" "${threads}" "${geomean}" "benchmark_tpch_validated" "${logfile}"
    log "OK ${q} t=${threads} geomean=${geomean}"
  done
  for q in Q1 Q3; do
    geomean=$(parse_validated_geomean "${q}" <"${logfile}" || true)
    if [[ -n "${geomean}" ]]; then
      append_geomean "${q}_validated" "${threads}" "${geomean}" "benchmark_tpch_validated" "${logfile}"
    fi
  done
}

wave_seed_filter() {
  local suffix=$1
  local seeds=()
  local i
  for i in $(seq 1 5); do seeds+=("seed_${i}_${suffix}"); done
  (IFS=,; echo "${seeds[*]}")
}

run_wave1_query() {
  local query=$1 threads=$2 filter=$3
  if already_done "${query}" "${threads}"; then
    log "SKIP ${query} t=${threads} (already recorded)"
    return 0
  fi
  local logfile="${OUT}/${query}_t${threads}.log"
  log "RUN ${query} t=${threads} (wave1 filter)"
  env ACCEL_BENCH_THREADS="${threads}" ACCEL_BENCH_FILTER="${filter}" \
    "${BUILD}/benchmark_tpch_wave1_parallel" --threads "${threads}" >"${logfile}" 2>&1 || true
  local geomean
  geomean=$(awk -F, -v q="${query}" '$1=="summary" && $2==q && $3=="geomean" {print $NF; exit}' "${logfile}")
  if [[ -z "${geomean}" ]]; then
    log "ERROR: wave1 missing ${query} t=${threads}"
    return 1
  fi
  append_geomean "${query}" "${threads}" "${geomean}" "benchmark_tpch_wave1_parallel" "${logfile}"
  log "OK ${query} t=${threads} geomean=${geomean}"
}

run_wave5_q20_q21() {
  local threads=$1
  local need=0
  for q in Q20 Q21; do
    if ! already_done "${q}" "${threads}"; then need=1; fi
  done
  if [[ "${need}" -eq 0 ]]; then
    log "SKIP Q20+Q21 t=${threads} (already recorded)"
    return 0
  fi
  local logfile="${OUT}/wave5_t${threads}.log"
  log "RUN Q20+Q21 t=${threads} (wave5)"
  env ACCEL_BENCH_THREADS="${threads}" ACCEL_BENCH_SEED_LIMIT=5 \
    "${BUILD}/benchmark_tpch_wave5_parallel" --threads "${threads}" >"${logfile}" 2>&1
  for q in Q20 Q21; do
    local geomean
    geomean=$(parse_geomean_from_log "${q}" <"${logfile}")
    if [[ -z "${geomean}" ]]; then
      log "ERROR: wave5 missing ${q} t=${threads}"
      return 1
    fi
    append_geomean "${q}" "${threads}" "${geomean}" "benchmark_tpch_wave5_parallel" "${logfile}"
    log "OK ${q} t=${threads} geomean=${geomean}"
  done
}

export ACCEL_TPCH_DB="${DB}"
export ACCEL_BENCH_WARMUP=3
export ACCEL_BENCH_ITERS=5
export ACCEL_BENCH_SEED_LIMIT=5
unset ACCEL_PARALLEL_PROFILE ACCEL_PARALLEL_AUDIT ACCEL_Q1_AUX_STATE 2>/dev/null || true
export DUCKDB_BITPACKING_NATIVE_FILTER=0

{
  echo "=== SF${SCALE} parallel 22-query matrix start $(date -Is) ==="
  echo "db=${DB} warmup=3 iters=5 seeds=5 q17_nonzero=10 threads=${THREAD_LIST}"

  cmake --build "${BUILD}" -j "$(nproc)" \
    --target benchmark_tpch_validated \
    benchmark_tpch_q1_parallel benchmark_tpch_q2_parallel benchmark_tpch_q3_parallel \
    benchmark_tpch_q5_parallel benchmark_tpch_q7_parallel benchmark_tpch_q8_parallel \
    benchmark_tpch_q10_parallel benchmark_tpch_q11_parallel benchmark_tpch_q13_parallel \
    benchmark_tpch_q16_parallel benchmark_tpch_q17_parallel benchmark_tpch_q18_parallel \
    benchmark_tpch_q22_parallel benchmark_tpch_wave1_parallel benchmark_tpch_wave5_parallel

  for threads in "${THREADS[@]}"; do
    log "=== threads=${threads} ==="

    run_individual Q1 benchmark_tpch_q1_parallel "${threads}"
    run_individual Q2 benchmark_tpch_q2_parallel "${threads}"
    run_individual Q3 benchmark_tpch_q3_parallel "${threads}"
    run_validated_subset "${threads}"
    run_individual Q5 benchmark_tpch_q5_parallel "${threads}"
    run_individual Q7 benchmark_tpch_q7_parallel "${threads}"
    run_individual Q8 benchmark_tpch_q8_parallel "${threads}"
    run_individual Q10 benchmark_tpch_q10_parallel "${threads}"
    run_individual Q11 benchmark_tpch_q11_parallel "${threads}"
    run_individual Q13 benchmark_tpch_q13_parallel "${threads}"
    run_individual Q16 benchmark_tpch_q16_parallel "${threads}"
    run_individual Q17 benchmark_tpch_q17_parallel "${threads}" "ACCEL_BENCH_Q17_NONZERO=1 ACCEL_BENCH_SEED_LIMIT=10"
    run_individual Q18 benchmark_tpch_q18_parallel "${threads}"
    run_individual Q22 benchmark_tpch_q22_parallel "${threads}"

    run_wave1_query Q15 "${threads}" "$(wave_seed_filter q15)"
    run_wave1_query Q19 "${threads}" "$(wave_seed_filter q19)"
    run_wave5_q20_q21 "${threads}"
  done

  echo "=== SF${SCALE} parallel 22-query matrix end $(date -Is) ==="
} 2>&1 | tee -a "${LOG}"

if [[ -f "${PLOT_SCRIPT}" ]]; then
  python3 "${PLOT_SCRIPT}" --csv "${CSV}"
else
  log "WARN: plot script not found: ${PLOT_SCRIPT}"
fi
