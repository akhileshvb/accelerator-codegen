#!/usr/bin/env bash
# Postgres matview vs aux write-mix sweep (composed workload at write %).
#
# For each query, measures read medians + maintenance costs, then composes
# workload_speedup at write_frequency_pct points (default 0,1,10,50).
# CSV includes aux_speedup_vs_matview per write %.
#
# Prerequisites:
#   Matviews applied (or this script applies them); extension registered.
#   Mutation capture function present (postgres_register_tpch_all.sql).
#
# Usage:
#   ACCEL_PG_ALLOW_SMALL_DB=1 TPCH_PG_DB=accel_tpch_sf1 \
#     bash scripts/run_tpch_pg_matview_vs_aux_write_mix_sweep.sh
#   bash scripts/run_tpch_pg_matview_vs_aux_write_mix_sweep.sh smoke   # Q6
#   WRITE_PCTS=0,10,50 QUERIES=1,6,14 bash scripts/run_tpch_pg_matview_vs_aux_write_mix_sweep.sh
#
# Outputs:
#   benchmarks/tpch/results_pg_sf20/matview_vs_aux_write_mix.csv  (override with OUT_DIR/CSV)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-full}"
DB="${TPCH_PG_DB:-accel_tpch_sf20}"
OUT_DIR="${OUT_DIR:-${ROOT}/benchmarks/tpch/results_pg_sf20}"
LOG="${LOG:-${OUT_DIR}/write_mix_sweep.log}"
CSV="${CSV:-${OUT_DIR}/matview_vs_aux_write_mix.csv}"
PYTHON="${ROOT}/.venv-pg-test/bin/python"
[[ -x "${PYTHON}" ]] || PYTHON=python3
NAME="${ACCEL_PG_CONTAINER:-accel_pg_test}"
EXTENSION_SO="${ACCEL_POSTGRES_SO:-/usr/lib/postgresql/17/lib/accelerator_codegen_pg.so}"
WRITE_PCTS="${WRITE_PCTS:-0,1,10,50}"
OPS="${WRITE_MIX_OPS:-100}"

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

echo "=== TPC-H Postgres matview vs aux WRITE-MIX sweep ===" | tee -a "${LOG}"
echo "db=${DB} queries=${QUERIES} write_pcts=${WRITE_PCTS} ops=${OPS} warmup=${WARMUP} samples=${SAMPLES}" | tee -a "${LOG}"

if ! psql -d "${DB}" -tAc "SELECT COUNT(*) FROM lineitem" >/tmp/accel_sf_li_count 2>/dev/null; then
  echo "ERROR: cannot query ${DB}.lineitem" >&2
  exit 1
fi
LI_COUNT="$(tr -d '[:space:]' </tmp/accel_sf_li_count)"
echo "lineitem_rows=${LI_COUNT}" | tee -a "${LOG}"
MIN_LI="${ACCEL_PG_MIN_LINEITEM:-50000000}"
if [[ "${ACCEL_PG_ALLOW_SMALL_DB:-0}" == "1" ]]; then
  MIN_LI=0
fi
if [[ "${LI_COUNT}" -lt "${MIN_LI}" ]]; then
  echo "ERROR: ${DB} looks too small (lineitem=${LI_COUNT}); expected >= ${MIN_LI}." >&2
  echo "For SF1: ACCEL_PG_ALLOW_SMALL_DB=1 TPCH_PG_DB=accel_tpch_sf1 bash $0 ..." >&2
  exit 1
fi

docker exec -e PGPASSWORD="${PGPASSWORD}" "${NAME}" \
  psql -v ON_ERROR_STOP=1 -U postgres -d "${DB}" \
  -c "LOAD '${EXTENSION_SO}';" >/dev/null

# Ensure capture function + shared TPC-H mutation triggers exist for aux ApplyDelta.
if ! psql -d "${DB}" -tAc "SELECT 1 FROM pg_proc WHERE proname = 'accel_aux_capture_transition'" | grep -q 1; then
  echo "ERROR: missing accel_aux_capture_transition — run postgres_register_tpch_all.sql" >&2
  exit 1
fi
echo "=== ensure mutation triggers ===" | tee -a "${LOG}"
bash "${ROOT}/scripts/ensure_accel_postgres_mutation_triggers.sh" "${DB}" \
  2>&1 | tee -a "${LOG}" || true

echo "=== ensure pg_ivm ===" | tee -a "${LOG}"
bash "${ROOT}/scripts/ensure_pg_ivm.sh" "${DB}" 2>&1 | tee -a "${LOG}"

IFS=',' read -r -a query_list <<< "${QUERIES}"
for query in "${query_list[@]}"; do
  echo "=== Q${query}: apply matview ===" | tee -a "${LOG}"
  if ! bash "${ROOT}/scripts/postgres_matview_apply.sh" "${query}" \
      >"${OUT_DIR}/q${query}_write_mix_apply.log" 2>&1; then
    echo "FAIL apply Q${query} (see ${OUT_DIR}/q${query}_write_mix_apply.log)" | tee -a "${LOG}"
    continue
  fi

  echo "=== Q${query}: apply IMMV (pg_ivm; may skip) ===" | tee -a "${LOG}"
  bash "${ROOT}/scripts/postgres_immv_apply.sh" "${query}" \
      >"${OUT_DIR}/q${query}_write_mix_immv_apply.log" 2>&1 \
    || echo "IMMV apply soft-fail Q${query} (see ${OUT_DIR}/q${query}_write_mix_immv_apply.log)" | tee -a "${LOG}"

  echo "=== Q${query}: write-mix bench pcts=${WRITE_PCTS} ===" | tee -a "${LOG}"
  if ! "${PYTHON}" "${ROOT}/scripts/tpch_pg_matview_vs_aux_bench.py" \
      --query "${query}" \
      --db "${DB}" \
      --warmup "${WARMUP}" \
      --samples "${SAMPLES}" \
      --write-pcts "${WRITE_PCTS}" \
      --ops "${OPS}" \
      --so "${EXTENSION_SO}" \
      --json-out "${OUT_DIR}/q${query}_write_mix.json" \
      --csv-out "${CSV}" \
      2>&1 | tee -a "${LOG}"; then
    echo "FAIL write-mix bench Q${query}" | tee -a "${LOG}"
    continue
  fi
done

echo "=== summarize write-mix (aux/mv and aux/ivm) ===" | tee -a "${LOG}"
if [[ -f "${CSV}" ]]; then
  "${PYTHON}" - "${CSV}" <<'PY' | tee -a "${LOG}"
import csv, sys
from pathlib import Path
from collections import defaultdict
p = Path(sys.argv[1])
rows = list(csv.DictReader(p.open()))
print(f"rows={len(rows)} csv={p}")

def f(r, k):
    try:
        v = r.get(k)
        return float(v) if v not in (None, "", "None") else None
    except ValueError:
        return None

by_q = defaultdict(list)
for r in rows:
    by_q[int(r["query"])].append(r)

pcts = sorted({int(float(r["write_frequency_pct"])) for r in rows})
hdr = f"{'Q':>3}" + "".join(f"  aux/mv@{p}%".rjust(14) for p in pcts) + "".join(f" aux/ivm@{p}%".rjust(14) for p in pcts)
print(hdr)
for q in sorted(by_q):
    recs = {int(float(r["write_frequency_pct"])): r for r in by_q[q]}
    cells = []
    for p in pcts:
        am = f(recs[p], "aux_speedup_vs_matview") if p in recs else None
        cells.append((f"{am:.2f}x" if am is not None else "nan").rjust(14))
    for p in pcts:
        ai = f(recs[p], "aux_speedup_vs_ivm") if p in recs else None
        cells.append((f"{ai:.2f}x" if ai is not None else "nan").rjust(14))
    print(f"{q:>3}" + "".join(cells))

print()
print(f"{'Q':>3} {'pct':>5} {'nat_wl':>12} {'mv_wl':>12} {'ivm_wl':>12} {'aux_wl':>12} {'mv_x':>8} {'ivm_x':>8} {'aux_x':>8} {'aux/mv':>8} {'aux/ivm':>8}")
for r in rows:
    q = r["query"]
    pct = r["write_frequency_pct"]
    def fmt(x, w):
        return (f"{x:.2f}" if x is not None else "nan").rjust(w)
    print(
        f"{q:>3} {pct:>5} "
        f"{fmt(f(r,'native_workload_ms'),12)} {fmt(f(r,'matview_workload_ms'),12)} "
        f"{fmt(f(r,'ivm_workload_ms'),12)} {fmt(f(r,'aux_workload_ms'),12)} "
        f"{fmt(f(r,'matview_speedup_vs_native'),8)} {fmt(f(r,'ivm_speedup_vs_native'),8)} "
        f"{fmt(f(r,'aux_speedup_vs_native'),8)} {fmt(f(r,'aux_speedup_vs_matview'),8)} "
        f"{fmt(f(r,'aux_speedup_vs_ivm'),8)}"
    )
PY
fi

echo "=== complete; csv=${CSV} log=${LOG} ===" | tee -a "${LOG}"
