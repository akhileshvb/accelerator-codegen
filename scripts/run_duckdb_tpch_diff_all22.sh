#!/usr/bin/env bash
# Run DuckDB TPC-H aux differentials (read + incremental + adversarial) for Q1..Q22.
# Optional: ACCEL_FORCE_MAINTENANCE_ACTION=ApplyDelta|RebuildPartition|RebuildAll
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD:-/tmp/accel_duckdb_build}"
BIN="${BIN:-${BUILD}/test_duckdb_tpch_differential}"
TPCH_DB="${ACCEL_TPCH_DB:-${ROOT}/../accelerator-codegen-duckdb/tpch_sf1.duckdb}"
FORCE="${ACCEL_FORCE_MAINTENANCE_ACTION:-}"
if [[ -n "${FORCE}" ]]; then
  OUT_DIR="${ACCEL_DUCKDB_DIFF_OUT:-${ROOT}/pipeline/artifacts/tpch/duckdb_sf1_diff_${FORCE}}"
else
  OUT_DIR="${ACCEL_DUCKDB_DIFF_OUT:-${ROOT}/pipeline/artifacts/tpch/duckdb_sf1_diff}"
fi
QUERIES="${ACCEL_DUCKDB_DIFF_QUERIES:-$(seq 1 22)}"

mkdir -p "${OUT_DIR}"
if [[ ! -x "${BIN}" ]]; then
  echo "missing binary: ${BIN}" >&2
  exit 1
fi
if [[ ! -f "${TPCH_DB}" ]]; then
  echo "missing DB: ${TPCH_DB}" >&2
  exit 1
fi

pass=0
fail=0
: > "${OUT_DIR}/all22.log"
echo "query,status" > "${OUT_DIR}/all22.csv"
echo "force=${FORCE:-none} bin=${BIN} db=${TPCH_DB}" | tee -a "${OUT_DIR}/all22.log"

for q in ${QUERIES}; do
  echo "=== Q${q} duckdb aux differential force=${FORCE:-none} ===" | tee -a "${OUT_DIR}/all22.log"
  set +e
  out=$(ACCEL_TPCH_DB="${TPCH_DB}" ACCEL_FORCE_MAINTENANCE_ACTION="${FORCE}" "${BIN}" "${q}" 2>&1)
  rc=$?
  set -e
  printf '%s\n' "${out}" | tee -a "${OUT_DIR}/all22.log" | tail -12
  if [[ ${rc} -eq 0 ]] && printf '%s\n' "${out}" | grep -q "PASS tpch Q${q} adversarial write differential"; then
    echo "PASS Q${q}"
    echo "${q},pass" >> "${OUT_DIR}/all22.csv"
    pass=$((pass + 1))
  else
    echo "FAIL Q${q} rc=${rc}"
    echo "${q},fail" >> "${OUT_DIR}/all22.csv"
    fail=$((fail + 1))
  fi
done

python3 - <<PY
import json
from pathlib import Path
out = Path("${OUT_DIR}")
rows = []
for line in (out / "all22.csv").read_text().splitlines()[1:]:
    if not line.strip():
        continue
    q, status = line.split(",", 1)
    rows.append({"query": int(q), "pass": status.strip() == "pass"})
n = len(rows)
failures = sum(1 for r in rows if not r["pass"])
summary = {
    "status": "pass" if failures == 0 and n == 22 else "fail",
    "failures": failures,
    "n": n,
    "force": "${FORCE}",
    "queries": rows,
    "db": "${TPCH_DB}",
    "binary": "${BIN}",
}
(out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
print(json.dumps(summary, indent=2))
PY

echo "summary pass=${pass} fail=${fail} force=${FORCE:-none} out=${OUT_DIR}"
exit "${fail}"
