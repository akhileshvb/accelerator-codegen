#!/usr/bin/env bash
# Collect SF1 scale-sensitive profiles for all 22 parallel TPC-H kernels.
# Usage: run_sf1_scale_audit.sh [threads]
# Profiles at t=1 and t=8 by default; writes to eval/bench/tpch/results/sf10_scale_audit/profiles/
set -euo pipefail

ROOT=/home/akhilvb/accelerator-codegen
BUILD="${ROOT}/build-release-parallel"
DB="${ACCEL_TPCH_DB:-/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb}"
OUT="${ROOT}/eval/bench/tpch/results/sf10_scale_audit/profiles"
LOG="${ROOT}/eval/bench/tpch/results/sf10_scale_audit/profile_campaign.log"
COMMIT=$(cd "${ROOT}" && git rev-parse HEAD)
THREADS="${1:-all}"

mkdir -p "${OUT}" "${ROOT}/eval/bench/tpch/results/sf10_scale_audit"
log() { echo "[$(date -Is)] $*" | tee -a "${LOG}"; }

log "=== SF1 scale audit profile collection ==="
log "commit=${COMMIT} db=${DB} build=${BUILD}"

if [[ ! -f "${DB}" ]]; then
  log "ERROR: database not found: ${DB}"
  exit 1
fi

run_profile() {
  local query=$1 bin=$2 threads=$3 extra_env=${4:-}
  local outfile="${OUT}/${query}_t${threads}.profile.log"
  if [[ -f "${outfile}" ]] && grep -q "parallel_profile\|accel_scale_profile" "${outfile}" 2>/dev/null; then
    log "SKIP ${query} t=${threads} (profile exists)"
    return 0
  fi
  log "PROFILE ${query} t=${threads} (${bin})"
  # shellcheck disable=SC2086
  env ACCEL_TPCH_DB="${DB}" ACCEL_BENCH_THREADS="${threads}" ACCEL_PARALLEL_PROFILE=1 ${extra_env} \
    "${BUILD}/${bin}" >"${outfile}" 2>&1 || {
    log "WARN ${query} t=${threads} exit=$? (see ${outfile})"
  }
}

THREAD_LIST=(1 8)
if [[ "${THREADS}" != "all" ]]; then
  THREAD_LIST=("${THREADS}")
fi

for t in "${THREAD_LIST[@]}"; do
  log "--- threads=${t} ---"

  # Individual parallel benchmarks
  run_profile Q1  benchmark_tpch_q1_parallel  "${t}"
  run_profile Q2  benchmark_tpch_q2_parallel  "${t}"
  run_profile Q3  benchmark_tpch_q3_parallel  "${t}"
  run_profile Q5  benchmark_tpch_q5_parallel  "${t}"
  run_profile Q7  benchmark_tpch_q7_parallel  "${t}"
  run_profile Q8  benchmark_tpch_q8_parallel  "${t}"
  run_profile Q10 benchmark_tpch_q10_parallel "${t}"
  run_profile Q11 benchmark_tpch_q11_parallel "${t}"
  run_profile Q13 benchmark_tpch_q13_parallel "${t}"
  run_profile Q16 benchmark_tpch_q16_parallel "${t}"
  run_profile Q17 benchmark_tpch_q17_parallel "${t}"
  run_profile Q18 benchmark_tpch_q18_parallel "${t}"
  run_profile Q22 benchmark_tpch_q22_parallel "${t}"

  # Wave benchmarks (Q15, Q19 via wave1; Q20, Q21 via wave5)
  run_profile Q15 benchmark_tpch_wave1_parallel "${t}"
  run_profile Q19 benchmark_tpch_wave1_parallel "${t}"
  run_profile Q20 benchmark_tpch_wave5_parallel "${t}"
  run_profile Q21 benchmark_tpch_wave5_parallel "${t}"

  # Validated subset (Q4, Q6, Q9, Q12, Q14)
  vfile="${OUT}/validated_t${t}.profile.log"
  if [[ ! -f "${vfile}" ]] || ! grep -q "parallel_profile" "${vfile}" 2>/dev/null; then
    log "PROFILE validated (Q4,Q6,Q9,Q12,Q14) t=${t}"
    env ACCEL_TPCH_DB="${DB}" ACCEL_BENCH_THREADS="${t}" ACCEL_PARALLEL_PROFILE=1 \
      "${BUILD}/benchmark_tpch_validated" --accelerator-mode parallel --threads "${t}" \
      >"${vfile}" 2>&1 || log "WARN validated t=${t}"
  else
    log "SKIP validated t=${t} (profile exists)"
  fi
done

log "=== profile collection complete ==="
log "Results in ${OUT}"
