#!/usr/bin/env bash
# Targeted-write isolation sweep: writer idle, round-robin, or single-query SQL invalidation.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build-release"
BIN="${BUILD}/benchmark_aux_cross_accelerator"
DB="${DB_PATH:-/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb}"
OUT="${OUT:-${BUILD}/cross_accel_targeted_write_sweep.csv}"
LOG_DIR="${BUILD}/cross_accel_targeted_sweep_logs"
DURATION="${ACCEL_CROSS_ACCEL_DURATION_SEC:-30}"
READERS="${ACCEL_CROSS_ACCEL_READERS:-5}"
QUERIES="${ACCEL_CROSS_ACCEL_QUERIES:-1,3,10,12,14}"
SLEEP_MS="${ACCEL_CROSS_ACCEL_WRITER_SLEEP_MS:-200}"
BULK_ROWS="${ACCEL_CROSS_ACCEL_WRITER_BULK_ROWS:-1}"

mkdir -p "$LOG_DIR"

if [[ ! -x "$BIN" ]]; then
  cmake --build "$BUILD" --target benchmark_aux_cross_accelerator -j"$(nproc)"
fi

# label|writer_prob_pct|writer_target
CONFIGS=(
  "A-idle|0|round_robin"
  "B-round-robin|100|round_robin"
  "C-target-Q1|100|1"
  "C-target-Q3|100|3"
  "C-target-Q10|100|10"
  "C-target-Q12|100|12"
  "C-target-Q14|100|14"
)

echo "sweep_label,writer_target,writer_prob_pct,effective_write_pct,accel_writer_commits,query_num,accel_p50_us,duckdb_p50_us,speedup_p50" >"$OUT"

for cfg in "${CONFIGS[@]}"; do
  IFS='|' read -r label prob_pct writer_target <<<"$cfg"
  log="${LOG_DIR}/${label}.log"
  echo "=== ${label} (target=${writer_target} prob=${prob_pct}%) ==="

  env_args=(
    "ACCEL_CROSS_ACCEL_QUERIES=${QUERIES}"
    "ACCEL_CROSS_ACCEL_READERS=${READERS}"
    "ACCEL_CROSS_ACCEL_DURATION_SEC=${DURATION}"
    "ACCEL_CROSS_ACCEL_WRITER_SLEEP_MS=${SLEEP_MS}"
    "ACCEL_CROSS_ACCEL_WRITER_BULK_ROWS=${BULK_ROWS}"
    "ACCEL_CROSS_ACCEL_WRITER_PROB_PCT=${prob_pct}"
    "ACCEL_CROSS_ACCEL_WRITER_TARGET=${writer_target}"
    "ACCEL_CROSS_ACCEL_SKIP_BUILD=0"
    "ACCEL_CROSS_ACCEL_SKIP_CORRECTNESS=1"
  )

  env "${env_args[@]}" "$BIN" "$DB" isolation 2>&1 | tee "$log" >/dev/null

  awk -F, -v label="$label" '
    /^isolation,/ && $12 ~ /^[0-9]+$/ {
      printf "%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
        label, $5, $9, $11, $7, $12, $15, $18, $19
    }
  ' "$log" >>"$OUT"
done

python3 "${ROOT}/scripts/summarize_targeted_write_sweep.py" "$OUT"
