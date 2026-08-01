#!/usr/bin/env bash
# Sweep effective write frequency using real invalidating SQL UPDATEs only.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build-release"
BIN="${BUILD}/benchmark_aux_cross_accelerator"
DB="${DB_PATH:-/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb}"
OUT="${OUT:-${BUILD}/cross_accel_write_freq_sweep.csv}"
LOG_DIR="${BUILD}/cross_accel_sweep_logs"
DURATION="${ACCEL_CROSS_ACCEL_DURATION_SEC:-20}"
DEFAULT_READERS="${ACCEL_CROSS_ACCEL_READERS:-5}"
QUERIES="${ACCEL_CROSS_ACCEL_QUERIES:-1,3,6,10,12,14}"

mkdir -p "$LOG_DIR"

if [[ ! -x "$BIN" ]]; then
  echo "building benchmark ..."
  cmake --build "$BUILD" --target benchmark_aux_cross_accelerator -j"$(nproc)"
fi

# label|disable_writer|readers|num_writers|sleep_ms|prob_pct
# All non-disabled points use per-query invalidating UPDATEs.
CONFIGS=(
  "0%|1|${DEFAULT_READERS}|0|0|0"
  "sql-sleep-5000|0|${DEFAULT_READERS}|1|5000|100"
  "sql-sleep-2000|0|${DEFAULT_READERS}|1|2000|100"
  "sql-sleep-1000|0|${DEFAULT_READERS}|1|1000|100"
  "sql-sleep-500|0|${DEFAULT_READERS}|1|500|100"
  "sql-sleep-200|0|${DEFAULT_READERS}|1|200|100"
  "sql-sleep-100|0|${DEFAULT_READERS}|1|100|100"
  "sql-sleep-50|0|${DEFAULT_READERS}|1|50|100"
  "sql-sleep-20|0|${DEFAULT_READERS}|1|20|100"
  "sql-sleep-10|0|${DEFAULT_READERS}|1|10|100"
  "sql-sleep-0|0|${DEFAULT_READERS}|1|0|100"
  "sql-1w-1r|0|1|1|0|100"
  "sql-2w-1r|0|1|2|0|100"
  "sql-4w-1r|0|1|4|0|100"
  "sql-8w-1r|0|1|8|0|100"
  "sql-16w-1r|0|1|16|0|100"
  "sql-32w-1r|0|1|32|0|100"
)

echo "write_freq_sweep_label,writer_style,readers,num_writers,writer_sleep_ms,writer_prob_pct,effective_write_pct,accel_writer_commits,total_reader_queries,query_num,accel_p50_us,duckdb_p50_us,speedup_p50" >"$OUT"

first=1
for cfg in "${CONFIGS[@]}"; do
  IFS='|' read -r label disable readers num_writers sleep_ms prob_pct <<<"$cfg"
  log="${LOG_DIR}/${label//\//_}.log"
  echo "=== ${label} (sql readers=${readers} writers=${num_writers} sleep_ms=${sleep_ms}) ==="

  env_args=(
    "ACCEL_CROSS_ACCEL_QUERIES=${QUERIES}"
    "ACCEL_CROSS_ACCEL_READERS=${readers}"
    "ACCEL_CROSS_ACCEL_NUM_WRITERS=${num_writers}"
    "ACCEL_CROSS_ACCEL_DURATION_SEC=${DURATION}"
    "ACCEL_CROSS_ACCEL_WRITER_SLEEP_MS=${sleep_ms}"
    "ACCEL_CROSS_ACCEL_WRITER_PROB_PCT=${prob_pct}"
    "ACCEL_CROSS_ACCEL_SKIP_CORRECTNESS=1"
  )
  if [[ "$disable" == "1" ]]; then
    env_args+=("ACCEL_CROSS_ACCEL_DISABLE_WRITER=1")
  fi
  if [[ "$first" == "1" ]]; then
    env_args+=("ACCEL_CROSS_ACCEL_SKIP_BUILD=0")
    first=0
  else
    env_args+=("ACCEL_CROSS_ACCEL_SKIP_BUILD=1")
  fi

  env "${env_args[@]}" "$BIN" "$DB" isolation 2>&1 | tee "$log" >/dev/null

  total_reader=$(awk -F, '/^isolation,/ && $12 ~ /^[0-9]+$/ {sum += $13} END {print sum+0}' "$log")
  awk -F, -v label="$label" -v readers="$readers" -v total_reader="$total_reader" '
    /^isolation,/ && $12 ~ /^[0-9]+$/ {
      printf "%s,sql,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
        label, readers, $4, $8, $9, $10, $7, total_reader, $12, $15, $18, $19
    }
  ' "$log" >>"$OUT"
done

python3 "${ROOT}/scripts/summarize_write_freq_sweep.py" "$OUT"
