#!/usr/bin/env bash
# SF20 Postgres sweep: native SQL vs matview residual vs postgres aux accelerators.
#
# Prerequisites (once):
#   bash scripts/setup_tpch_sf20_postgres.sh
#
# Usage:
#   bash scripts/run_tpch_pg_matview_vs_aux_sf20_sweep.sh           # all 22
#   bash scripts/run_tpch_pg_matview_vs_aux_sf20_sweep.sh smoke     # Q6 only
#   QUERIES=1,6,14 bash scripts/run_tpch_pg_matview_vs_aux_sf20_sweep.sh
#
# Outputs:
#   benchmarks/tpch/results_pg_sf20/matview_vs_aux_sf20.csv
#   benchmarks/tpch/results_pg_sf20/q{N}.json
#   benchmarks/tpch/results_pg_sf20/sweep.log
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-full}"
DB="${TPCH_PG_DB:-accel_tpch_sf20}"
OUT_DIR="${OUT_DIR:-${ROOT}/benchmarks/tpch/results_pg_sf20}"
LOG="${LOG:-${OUT_DIR}/sweep.log}"
CSV="${CSV:-${OUT_DIR}/matview_vs_aux_sf20.csv}"
PYTHON="${ROOT}/.venv-pg-test/bin/python"
[[ -x "${PYTHON}" ]] || PYTHON=python3
NAME="${ACCEL_PG_CONTAINER:-accel_pg_test}"
EXTENSION_SO="${ACCEL_POSTGRES_SO:-/usr/lib/postgresql/17/lib/accelerator_codegen_pg.so}"

export PGPASSWORD="${PGPASSWORD:-postgres}"
export PGHOST="${PG_HOST:-127.0.0.1}"
export PGPORT="${PG_PORT:-${PGPORT:-5434}}"
export PGUSER="${PGUSER:-postgres}"
export TPCH_PG_DB="${DB}"

WARMUP="${ACCEL_PG_BENCH_WARMUP:-2}"
SAMPLES="${ACCEL_PG_BENCH_SAMPLES:-5}"
if [[ "${MODE}" == "smoke" ]]; then
  QUERIES="${QUERIES:-6}"
  WARMUP="${ACCEL_PG_BENCH_WARMUP:-1}"
  SAMPLES="${ACCEL_PG_BENCH_SAMPLES:-2}"
else
  QUERIES="${QUERIES:-1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22}"
fi

mkdir -p "${OUT_DIR}"
: >"${LOG}"
rm -f "${CSV}"

echo "=== TPC-H SF20 Postgres matview vs aux sweep ===" | tee -a "${LOG}"
echo "db=${DB} queries=${QUERIES} warmup=${WARMUP} samples=${SAMPLES}" | tee -a "${LOG}"

if ! psql -d "${DB}" -tAc "SELECT COUNT(*) FROM lineitem" >/tmp/accel_sf20_li_count 2>/dev/null; then
  echo "ERROR: cannot query ${DB}.lineitem — run scripts/setup_tpch_sf20_postgres.sh first" >&2
  exit 1
fi
LI_COUNT="$(tr -d '[:space:]' </tmp/accel_sf20_li_count)"
echo "lineitem_rows=${LI_COUNT}" | tee -a "${LOG}"
# SF20 lineitem is ~120M+; SF1 is ~6M. Override with ACCEL_PG_ALLOW_SMALL_DB=1 or
# ACCEL_PG_MIN_LINEITEM (default 50000000 for SF20 sweeps).
MIN_LI="${ACCEL_PG_MIN_LINEITEM:-50000000}"
if [[ "${ACCEL_PG_ALLOW_SMALL_DB:-0}" == "1" ]]; then
  MIN_LI=0
fi
if [[ "${LI_COUNT}" -lt "${MIN_LI}" ]]; then
  echo "ERROR: ${DB} looks too small (lineitem=${LI_COUNT}); expected >= ${MIN_LI}." >&2
  echo "For SF1: ACCEL_PG_ALLOW_SMALL_DB=1 TPCH_PG_DB=accel_tpch_sf1 bash $0 ..." >&2
  exit 1
fi

# Ensure extension is loadable in this DB.
docker exec -e PGPASSWORD="${PGPASSWORD}" "${NAME}" \
  psql -v ON_ERROR_STOP=1 -U postgres -d "${DB}" \
  -c "LOAD '${EXTENSION_SO}';" >/dev/null

echo "=== ensure pg_ivm ===" | tee -a "${LOG}"
bash "${ROOT}/scripts/ensure_pg_ivm.sh" "${DB}" 2>&1 | tee -a "${LOG}"

IFS=',' read -r -a query_list <<< "${QUERIES}"
for query in "${query_list[@]}"; do
  echo "=== Q${query}: apply matview ===" | tee -a "${LOG}"
  if ! bash "${ROOT}/scripts/postgres_matview_apply.sh" "${query}" \
      >"${OUT_DIR}/q${query}_apply.log" 2>&1; then
    echo "FAIL apply Q${query} (see ${OUT_DIR}/q${query}_apply.log)" | tee -a "${LOG}"
    continue
  fi

  echo "=== Q${query}: apply IMMV (pg_ivm; may skip) ===" | tee -a "${LOG}"
  bash "${ROOT}/scripts/postgres_immv_apply.sh" "${query}" \
      >"${OUT_DIR}/q${query}_immv_apply.log" 2>&1 \
    || echo "IMMV apply soft-fail Q${query}" | tee -a "${LOG}"

  echo "=== Q${query}: bench native / matview / ivm / aux ===" | tee -a "${LOG}"
  if ! "${PYTHON}" "${ROOT}/scripts/tpch_pg_matview_vs_aux_bench.py" \
      --query "${query}" \
      --db "${DB}" \
      --warmup "${WARMUP}" \
      --samples "${SAMPLES}" \
      --so "${EXTENSION_SO}" \
      --json-out "${OUT_DIR}/q${query}.json" \
      --csv-out "${CSV}" \
      2>&1 | tee -a "${LOG}"; then
    echo "FAIL bench Q${query}" | tee -a "${LOG}"
    continue
  fi
done

echo "=== summarize ===" | tee -a "${LOG}"
if [[ -f "${CSV}" ]]; then
  "${PYTHON}" - "${CSV}" <<'PY' | tee -a "${LOG}"
import csv, sys
from pathlib import Path
p = Path(sys.argv[1])
rows = list(csv.DictReader(p.open()))
print(f"rows={len(rows)} csv={p}")
print(f"{'Q':>3} {'native_ms':>12} {'mv_ms':>12} {'aux_ms':>12} {'mv_x':>8} {'aux_x':>8} {'aux/mv':>8}")
for r in rows:
    q = r["query"]
    def f(k):
        try:
            return float(r[k]) if r.get(k) not in (None, "", "None") else None
        except ValueError:
            return None
    native, mv, aux = f("native_median_ms"), f("matview_median_ms"), f("aux_median_ms")
    mvx, auxx, am = f("matview_speedup_vs_native"), f("aux_speedup_vs_native"), f("aux_speedup_vs_matview")
    def fmt(x, w):
        return (f"{x:.2f}" if x is not None else "nan").rjust(w)
    print(f"{q:>3} {fmt(native,12)} {fmt(mv,12)} {fmt(aux,12)} {fmt(mvx,8)} {fmt(auxx,8)} {fmt(am,8)}")
PY
fi

echo "=== complete; csv=${CSV} log=${LOG} ===" | tee -a "${LOG}"
