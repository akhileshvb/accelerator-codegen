#!/usr/bin/env bash
# MySQL TPC-H aux sweep → shared-interface CSV (same columns as
# eval/bench/aux_state/results/{postgres,duckdb}_*_sweep.csv).
#
# Default: SF=10 container, read-only (interval 0) then 10% point write-mix
# (labeled write_interval_sec=2.0 to match DuckDB/Postgres 8-read / 1-write).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONTAINER="${ACCEL_MYSQL_CONTAINER:-accel_mysql_sf10}"
# Refuse to measure the test variant of the plugin (it carries the gate-test UDFs).
if [[ "$(docker exec "$CONTAINER" sh -c 'cat /usr/lib/mysql/plugin/accelerator_codegen_mysql.variant /usr/lib64/mysql/plugin/accelerator_codegen_mysql.variant 2>/dev/null' | head -1)" == "test" ]]; then
  echo "$CONTAINER has the TEST variant of accelerator_codegen_mysql installed; rebuild with backends/mysql/host.sh build" >&2
  exit 1
fi
QUERIES="${ACCEL_MYSQL_SWEEP_QUERIES:-$(seq 1 22)}"
SCALE="${ACCEL_MYSQL_SWEEP_SCALE:-sf10}"
OUT="${ACCEL_SHARED_SWEEP_OUTPUT:-${ROOT}/eval/bench/aux_state/results/mysql_tpch_${SCALE}_aux_sweep.csv}"
LOG="${ACCEL_MYSQL_SWEEP_LOG:-${OUT%.csv}.log}"
RUN_RO="${ACCEL_MYSQL_SWEEP_READONLY:-1}"
RUN_MIX="${ACCEL_MYSQL_SWEEP_WRITE_MIX:-1}"
WRITE_PCT="${ACCEL_MYSQL_SWEEP_WRITE_PCT:-10}"
MIX_INTERVAL="${ACCEL_SHARED_SWEEP_INTERVAL:-2.0}"
FORCE_ACTION="${ACCEL_FORCE_MAINTENANCE_ACTION:-ApplyDelta}"

if [[ "$OUT" == "$ROOT"/* ]]; then
  CONTAINER_OUT="/accel-src/${OUT#"$ROOT"/}"
else
  CONTAINER_OUT="/accel-src/eval/bench/aux_state/results/mysql_tpch_${SCALE}_aux_sweep.csv"
  OUT="${ROOT}/eval/bench/aux_state/results/mysql_tpch_${SCALE}_aux_sweep.csv"
fi

mkdir -p "$(dirname "$OUT")"
: >"$LOG"
if [[ "${ACCEL_MYSQL_SWEEP_RESUME:-0}" != "1" || ! -s "$OUT" ]]; then
  echo "engine,workload,query,write_interval_sec,mode,reads,writes,rows,read_p50_us,read_sum_us,write_p50_us,write_sum_us,build_us,workload_sum_us,workload_speedup_vs_base,workload_with_build_us,workload_with_build_speedup_vs_base" >"$OUT"
fi

run_one() {
  local q="$1"
  local write_pct="$2"
  local extra_env=()
  if [[ "$write_pct" != "0" ]]; then
    extra_env+=(-e "ACCEL_FORCE_MAINTENANCE_ACTION=${FORCE_ACTION}")
    extra_env+=(-e "ACCEL_SHARED_SWEEP_INTERVAL=${MIX_INTERVAL}")
  fi
  echo "=== Q${q} write_pct=${write_pct} $(date -Iseconds) ===" | tee -a "$LOG"
  local start elapsed rc
  start=$(date +%s)
  set +e
  docker exec \
    -e MYSQL_PWD=mysql \
    -e MYSQL_HOST=127.0.0.1 \
    -e MYSQL_PORT=3306 \
    -e TPCH_MYSQL_DB=accel_mysql_test \
    -e ACCEL_SRC=/accel-src \
    -e ACCELERATOR_CODEGEN_CONFIG_DIR=/accel-src/config \
    -e ACCEL_MYSQL_SWEEP_SCALE="${SCALE}" \
    -e ACCEL_TPCH_Q11_FRACTION="${ACCEL_TPCH_Q11_FRACTION:-}" \
    -e ACCEL_SHARED_SWEEP_OUTPUT="${CONTAINER_OUT}" \
    -e ACCEL_SHARED_SWEEP_APPEND=1 \
    -e ACCEL_SHARED_SWEEP_READS="${ACCEL_SHARED_SWEEP_READS:-8}" \
    -e ACCEL_SHARED_SWEEP_WARMUP="${ACCEL_SHARED_SWEEP_WARMUP:-1}" \
    "${extra_env[@]}" \
    -w /accel-build/accel \
    "$CONTAINER" \
    ./benchmark_mysql_tpch_aux "$q" "$write_pct" | tee -a "$LOG"
  rc=${PIPESTATUS[0]}
  set -e
  elapsed=$(( $(date +%s) - start ))
  echo "Q${q} write_pct=${write_pct} elapsed=${elapsed}s rc=${rc}" | tee -a "$LOG"
  return "$rc"
}

fail=0
for q in $QUERIES; do
  if [[ "$RUN_RO" == "1" ]]; then
    run_one "$q" 0 || fail=$((fail + 1))
  fi
  if [[ "$RUN_MIX" == "1" ]]; then
    run_one "$q" "$WRITE_PCT" || fail=$((fail + 1))
  fi
done

echo "=== summarizing ${OUT} ===" | tee -a "$LOG"
ACCEL_SWEEP_DATASET="tpch_${SCALE}" \
  python3 "${ROOT}/scripts/summarize_shared_interface_sweep.py" "$OUT" \
  --summary "${OUT%.csv}_summary.csv" | tee -a "$LOG"

echo "wrote ${OUT}"
echo "summary ${OUT%.csv}_summary.csv"
if [[ "$fail" -ne 0 ]]; then
  echo "${fail} query/mode(s) failed; see ${LOG}" >&2
  exit 1
fi
