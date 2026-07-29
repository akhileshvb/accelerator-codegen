#!/usr/bin/env bash
# Writer→reader interference matrix: concurrent multi-accelerator readers while a
# background writer runs invalidating SQL UPDATEs for one accelerator at a time.
#
# Outputs CSV + heatmap showing per-query isolation under cross-accelerator writes.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${ROOT}/build-release"
BIN="${BUILD}/benchmark_aux_cross_accelerator"
DB="${DB_PATH:-/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb}"
OUT="${OUT:-${BUILD}/cross_accel_interference_matrix.csv}"
PNG="${PNG:-${BUILD}/cross_accel_interference_matrix.png}"
LOG_DIR="${BUILD}/cross_accel_interference_logs"
DURATION="${ACCEL_CROSS_ACCEL_DURATION_SEC:-30}"
READERS="${ACCEL_CROSS_ACCEL_READERS:-5}"
QUERIES="${ACCEL_CROSS_ACCEL_QUERIES:-1,3,6,10,12,14}"
NUM_WRITERS="${ACCEL_CROSS_ACCEL_NUM_WRITERS:-1}"
SLEEP_MS="${ACCEL_CROSS_ACCEL_WRITER_SLEEP_MS:-200}"

mkdir -p "$LOG_DIR"

if [[ ! -x "$BIN" ]]; then
  cmake --build "$BUILD" --target benchmark_aux_cross_accelerator -j"$(nproc)"
fi

# writer_label|writer_target|disable_writer
CONFIGS=(
  "idle|round_robin|1"
  "round-robin|round_robin|0"
  "target-Q1|1|0"
  "target-Q3|3|0"
  "target-Q10|10|0"
  "target-Q12|12|0"
  "target-Q14|14|0"
  "target-Q6|6|0"
)

echo "writer_label,writer_target,writer_commits,effective_write_pct,reader_query,accel_p50_us,duckdb_p50_us,speedup_p50,retention_vs_idle" >"$OUT"

for cfg in "${CONFIGS[@]}"; do
  IFS='|' read -r label target disable <<<"$cfg"
  log="${LOG_DIR}/${label}.log"
  echo "=== ${label} (writer_target=${target}) ==="

  env_args=(
    "ACCEL_CROSS_ACCEL_QUERIES=${QUERIES}"
    "ACCEL_CROSS_ACCEL_READERS=${READERS}"
    "ACCEL_CROSS_ACCEL_DURATION_SEC=${DURATION}"
    "ACCEL_CROSS_ACCEL_WRITER_SLEEP_MS=${SLEEP_MS}"
    "ACCEL_CROSS_ACCEL_WRITER_PROB_PCT=100"
    "ACCEL_CROSS_ACCEL_WRITER_TARGET=${target}"
    "ACCEL_CROSS_ACCEL_NUM_WRITERS=${NUM_WRITERS}"
    "ACCEL_CROSS_ACCEL_SKIP_CORRECTNESS=1"
    "ACCEL_CROSS_ACCEL_SKIP_BASELINE=1"
    "ACCEL_CROSS_ACCEL_SKIP_BUILD=0"
  )
  if [[ "$disable" == "1" ]]; then
    env_args+=("ACCEL_CROSS_ACCEL_DISABLE_WRITER=1" "ACCEL_CROSS_ACCEL_NUM_WRITERS=0")
  fi

  env "${env_args[@]}" "$BIN" "$DB" isolation 2>&1 | tee "$log" >/dev/null

  awk -F, -v label="$label" '
    /^isolation,/ && $11 ~ /^[0-9]+$/ {
      printf "%s,%s,%s,%s,%s,%s,%s,%s,\n",
        label, $5, $7, $10, $11, $14, $17, $18
    }
  ' "$log" >>"$OUT"
done

python3 "${ROOT}/scripts/plot_interference_matrix.py" "$OUT" "$PNG"
python3 "${ROOT}/scripts/summarize_interference_matrix.py" "$OUT"
