#!/usr/bin/env bash
# Cross-accelerator sweep with aux_bench-aligned scheduled write % (0/1/10/50).
# Each reader connection invalidates its own query's partitions on the same
# schedule as benchmark_tpch_q{N}_aux before running concurrent reads.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build-release"
BIN="${BUILD}/benchmark_aux_cross_accelerator"
DB="${DB_PATH:-/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb}"
OUT="${OUT:-${BUILD}/cross_accel_scheduled_write_sweep.csv}"
LOG_DIR="${BUILD}/cross_accel_scheduled_sweep_logs"
DURATION="${ACCEL_CROSS_ACCEL_DURATION_SEC:-30}"
READERS="${ACCEL_CROSS_ACCEL_READERS:-5}"
QUERIES="${ACCEL_CROSS_ACCEL_QUERIES:-1,3,6,10,12,14}"
FREQUENCIES="${ACCEL_CROSS_ACCEL_SCHEDULED_FREQUENCIES:-0,1,10,50}"

mkdir -p "$LOG_DIR"

if [[ ! -x "$BIN" ]]; then
  cmake --build "$BUILD" --target benchmark_aux_cross_accelerator -j"$(nproc)"
fi

echo "write_freq_sweep_label,writer_style,readers,scheduled_write_pct,effective_write_pct,accel_writer_commits,total_reader_queries,query_num,accel_p50_us,duckdb_p50_us,speedup_p50" >"$OUT"

first=1
IFS=',' read -r -a freq_list <<< "${FREQUENCIES}"
for freq in "${freq_list[@]}"; do
  label="${freq}%"
  log="${LOG_DIR}/${freq}pct.log"
  echo "=== scheduled ${label} (readers=${READERS}) ==="

  env_args=(
    "ACCEL_CROSS_ACCEL_QUERIES=${QUERIES}"
    "ACCEL_CROSS_ACCEL_READERS=${READERS}"
    "ACCEL_CROSS_ACCEL_DURATION_SEC=${DURATION}"
    "ACCEL_CROSS_ACCEL_SCHEDULED_WRITE_PCT=${freq}"
    "ACCEL_CROSS_ACCEL_DISABLE_WRITER=1"
    "ACCEL_CROSS_ACCEL_SKIP_CORRECTNESS=1"
  )
  if [[ "$first" == "1" ]]; then
    env_args+=("ACCEL_CROSS_ACCEL_SKIP_BUILD=0")
    first=0
  else
    env_args+=("ACCEL_CROSS_ACCEL_SKIP_BUILD=1")
  fi

  env "${env_args[@]}" "$BIN" "$DB" isolation 2>&1 | tee "$log" >/dev/null

  total_reader=$(awk -F, '/^isolation,/ && $12 ~ /^[0-9]+$/ {sum += $13} END {print sum+0}' "$log")
  awk -F, -v label="$label" -v readers="$READERS" -v total_reader="$total_reader" '
    /^isolation,/ && $12 ~ /^[0-9]+$/ {
      printf "%s,scheduled,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
        label, readers, $10, $11, $7, total_reader, $12, $15, $18, $19
    }
  ' "$log" >>"$OUT"
done

python3 "${ROOT}/scripts/summarize_scheduled_write_sweep.py" "$OUT"
