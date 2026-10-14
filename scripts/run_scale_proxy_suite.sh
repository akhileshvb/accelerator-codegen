#!/usr/bin/env bash
# Run scale-proxy microbenchmarks for SF10 scalability audit.
set -euo pipefail

ROOT=/home/akhilvb/accelerator-codegen
BUILD="${ROOT}/build-release-parallel"
OUT="${ROOT}/eval/bench/tpch/results/sf10_scale_audit/proxies"
LOG="${OUT}/proxy_campaign.log"
PROXY="${BUILD}/benchmark_tpch_scale_proxy"

mkdir -p "${OUT}" "${ROOT}/eval/bench/tpch/results/sf10_scale_audit"
log() { echo "[$(date -Is)] $*" | tee -a "${LOG}"; }

if [[ ! -x "${PROXY}" ]]; then
  log "ERROR: ${PROXY} not found; build first"
  exit 1
fi

log "=== scale proxy suite start ==="

run_proxy() {
  local label=$1
  shift
  local outfile="${OUT}/${label}.log"
  if [[ -f "${outfile}" ]] && grep -q "scale_proxy_done" "${outfile}" 2>/dev/null; then
    log "SKIP ${label} (done)"
    return 0
  fi
  log "RUN ${label}"
  "$@" >"${outfile}" 2>&1
  log "OK ${label}"
}

# Proxy 2: state amplification at SF1 and projected SF10 sizes
for domain in order customer part; do
  for mult in 1 10; do
    for threads in 1 8; do
      run_proxy "state_amp_${domain}_sf${mult}_t${threads}" \
        env ACCEL_SCALE_PROXY=state_amplification ACCEL_SCALE_DOMAIN="${domain}" \
            ACCEL_SCALE_MULTIPLIER="${mult}" ACCEL_BENCH_THREADS="${threads}" \
            ACCEL_SCALE_PROXY_ITERS=5 "${PROXY}"
    done
  done
done

# Proxy 4: cache stress at multiple state sizes
for domain in order customer; do
  for mult in 1 10; do
    run_proxy "cache_stress_${domain}_sf${mult}" \
      env ACCEL_SCALE_PROXY=cache_stress ACCEL_SCALE_DOMAIN="${domain}" \
          ACCEL_SCALE_MULTIPLIER="${mult}" ACCEL_SCALE_PROXY_ITERS=5 \
          ACCEL_SCALE_PROBE_COUNT=10000000 "${PROXY}"
  done
done

# Proxy 5: merge stress
for mult in 1 2 5 10; do
  for threads in 1 4 8 16; do
    run_proxy "merge_stress_sf${mult}_t${threads}" \
      env ACCEL_SCALE_PROXY=merge_stress ACCEL_SCALE_MULTIPLIER="${mult}" \
          ACCEL_BENCH_THREADS="${threads}" ACCEL_SCALE_PROXY_ITERS=5 "${PROXY}"
  done
done

# Proxy 3: materialization amplification
for mult in 1 2 5 10; do
  for threads in 1 8; do
    run_proxy "materialization_sf${mult}_t${threads}" \
      env ACCEL_SCALE_PROXY=materialization ACCEL_SCALE_MULTIPLIER="${mult}" \
          ACCEL_BENCH_THREADS="${threads}" ACCEL_SCALE_PROXY_ITERS=5 \
          ACCEL_SCALE_RECORD_COUNT=500000 "${PROXY}"
  done
done

log "=== scale proxy suite complete ==="
