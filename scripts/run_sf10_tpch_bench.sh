#!/usr/bin/env bash
# One-shot SF10 TPC-H speedup suite for Q1, Q3, Q4, Q6, Q9, Q12, Q14.
#
# Leaves SF1 as the default for all other scripts/tests. Points only this run at
# tpch_sf10.duckdb, filters to seeds 1-5, and enforces disk/RAM preflight gates.
set -euo pipefail

ROOT=/home/akhilvb/accelerator-codegen
DUCKDB_ROOT=/home/akhilvb/accelerator-codegen-duckdb
BUILD="${ROOT}/build-release"
OUT_DIR="${ROOT}/benchmarks/tpch/results"
SF10_DB="${DUCKDB_ROOT}/tpch_sf10.duckdb"
MIN_DISK_GB=20
MIN_MEM_GB=16
MAX_SMOKE_RSS_GB=32

seed_filter() {
  local q="$1"
  local parts=()
  local i
  for i in 1 2 3 4 5; do
    parts+=("seed_${i}_q${q}")
  done
  local IFS=,
  echo "${parts[*]}"
}

preflight() {
  local free_gb mem_gb
  free_gb=$(df -BG "${DUCKDB_ROOT}" | awk 'NR==2 {gsub(/G/,"",$4); print $4}')
  mem_gb=$(awk '/MemAvailable/ {printf "%d", $2/1024/1024}' /proc/meminfo)
  echo "preflight_free_disk_gb=${free_gb}"
  echo "preflight_mem_available_gb=${mem_gb}"
  if [ "${free_gb}" -lt "${MIN_DISK_GB}" ]; then
    echo "ERROR: need >= ${MIN_DISK_GB} GB free disk, have ${free_gb} GB" >&2
    exit 1
  fi
  if [ "${mem_gb}" -lt "${MIN_MEM_GB}" ]; then
    echo "ERROR: need >= ${MIN_MEM_GB} GB MemAvailable, have ${mem_gb} GB" >&2
    exit 1
  fi
  if [ ! -f "${SF10_DB}" ]; then
    echo "ERROR: missing ${SF10_DB}" >&2
    exit 1
  fi
  echo "preflight_ok=1"
}

run_bench() {
  local q="$1"
  local mode="${2:-full}" # smoke | full
  local bin="${BUILD}/benchmark_tpch_q${q}"
  local filter
  if [ "${mode}" = smoke ]; then
    filter="seed_1_q${q}"
  else
    filter="$(seed_filter "${q}")"
  fi
  local csv="${OUT_DIR}/sf10_q${q}.csv"
  local log="${OUT_DIR}/sf10_q${q}.log"
  if [ "${mode}" = smoke ]; then
    csv="${OUT_DIR}/sf10_smoke_q${q}.csv"
    log="${OUT_DIR}/sf10_smoke_q${q}.log"
  fi

  echo "=== q${q} mode=${mode} filter=${filter} -> ${csv} ==="
  echo "mem_available_gb_before=$(awk '/MemAvailable/ {printf "%d", $2/1024/1024}' /proc/meminfo)"

  local warmup iters
  if [ "${mode}" = smoke ]; then
    # Correctness + RSS probe only; full suite uses the standard 3/10 methodology.
    warmup=1
    iters=1
  else
    warmup="${ACCEL_BENCH_WARMUP}"
    iters="${ACCEL_BENCH_ITERS}"
  fi

  # Sample peak RSS via /proc (GNU time is not installed on this host).
  local peak_file="${log}.peak_rss_kb"
  echo 0 >"${peak_file}"
  set +e
  ACCEL_BENCH_FILTER="${filter}" \
  ACCEL_BENCH_WARMUP="${warmup}" \
  ACCEL_BENCH_ITERS="${iters}" \
    "${bin}" >"${csv}" 2>"${log}.err" &
  local bench_pid=$!
  (
    max_kb=0
    while kill -0 "${bench_pid}" 2>/dev/null; do
      kb=$(awk '/VmRSS:/ {print $2}' "/proc/${bench_pid}/status" 2>/dev/null || echo 0)
      if [ "${kb}" -gt "${max_kb}" ]; then
        max_kb="${kb}"
        echo "${max_kb}" >"${peak_file}"
      fi
      sleep 0.25
    done
  ) &
  local mon_pid=$!
  wait "${bench_pid}"
  local rc=$?
  wait "${mon_pid}" 2>/dev/null || true
  set -e

  {
    echo "exit_code=${rc}"
    if [ -s "${log}.err" ]; then
      cat "${log}.err"
    fi
  } >"${log}"

  local max_rss_kb max_rss_gb
  max_rss_kb=$(cat "${peak_file}")
  if [ -z "${max_rss_kb}" ]; then
    max_rss_kb=0
  fi
  max_rss_gb=$(( max_rss_kb / 1024 / 1024 ))
  echo "peak_rss_kb=${max_rss_kb} peak_rss_gb=${max_rss_gb}"
  echo "peak_rss_kb=${max_rss_kb}" >>"${log}"
  echo "peak_rss_gb=${max_rss_gb}" >>"${log}"

  if [ "${rc}" -ne 0 ]; then
    echo "ERROR: benchmark_tpch_q${q} failed (rc=${rc}); see ${log}" >&2
    tail -n 50 "${log}" >&2 || true
    exit "${rc}"
  fi

  if [ "${mode}" = smoke ] && [ "${max_rss_gb}" -gt "${MAX_SMOKE_RSS_GB}" ]; then
    echo "ERROR: smoke q${q} peak RSS ${max_rss_gb} GB exceeds ${MAX_SMOKE_RSS_GB} GB; aborting full suite" >&2
    exit 1
  fi

  # Print summary lines from CSV
  if [ -f "${csv}" ]; then
    grep -E 'seed_|summary' "${csv}" | tail -n 30 || true
  fi
}

mkdir -p "${OUT_DIR}"

echo "=== SF10 TPC-H benchmark ==="
echo "accelerator_commit=$(git -C "${ROOT}" rev-parse HEAD 2>/dev/null || echo unknown)"
echo "duckdb_commit=$(git -C "${DUCKDB_ROOT}" rev-parse HEAD 2>/dev/null || echo unknown)"
echo "ACCEL_TPCH_DB will be ${SF10_DB}"
echo "sf10_db_size=$(ls -lh "${SF10_DB}" | awk '{print $5}')"
echo "sf1_untouched=$(stat -c '%y %s' "${DUCKDB_ROOT}/tpch_sf1.duckdb")"

preflight

# Drop inherited experimental toggles.
while IFS= read -r name; do
  unset "${name}" || true
done < <(env | awk -F= '/^ACCEL_Q|^DUCKDB_BITPACKING|^DUCKDB_Q6_/ {print $1}')

export ACCEL_TPCH_DB="${SF10_DB}"
export ACCEL_BENCH_WARMUP=3
export ACCEL_BENCH_ITERS=10
export DUCKDB_BITPACKING_NATIVE_FILTER=0
export ACCEL_Q1_AUX_STATE=0

TARGETS=(benchmark_tpch_q1 benchmark_tpch_q3 benchmark_tpch_q4 benchmark_tpch_q6
         benchmark_tpch_q9 benchmark_tpch_q12 benchmark_tpch_q14)
echo "=== building targets ==="
cmake --build "${BUILD}" -j "$(nproc)" --target "${TARGETS[@]}"

MODE="${1:-all}" # smoke | full | all

QUERIES=(1 3 4 6 9 12 14)

if [ "${MODE}" = smoke ] || [ "${MODE}" = all ]; then
  echo "=== smoke seed_1 (memory-sensitive first: q3, q9) ==="
  for q in 3 9 1 4 6 12 14; do
    run_bench "${q}" smoke
  done
  echo "=== smoke complete ==="
fi

if [ "${MODE}" = full ] || [ "${MODE}" = all ]; then
  echo "=== full seeds 1-5 ==="
  SUMMARY="${OUT_DIR}/sf10_summary.log"
  : >"${SUMMARY}"
  {
    echo "sf10_db=${SF10_DB}"
    echo "warmup=${ACCEL_BENCH_WARMUP} iters=${ACCEL_BENCH_ITERS}"
    date -Is
  } >>"${SUMMARY}"

  for q in "${QUERIES[@]}"; do
    run_bench "${q}" full
    {
      echo "--- q${q} ---"
      grep -E 'summary|peak_rss' "${OUT_DIR}/sf10_q${q}.log" || true
      grep -E 'summary_avg_median|summary' "${OUT_DIR}/sf10_q${q}.csv" || true
      # Prefer last summary line; also echo avg speedup if present
      awk -F, '/^summary/ {print}' "${OUT_DIR}/sf10_q${q}.csv" || true
    } >>"${SUMMARY}"
  done

  echo "=== full suite complete; summary at ${SUMMARY} ==="
  cat "${SUMMARY}"
fi
