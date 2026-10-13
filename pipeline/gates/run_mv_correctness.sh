#!/usr/bin/env bash
# Compare residual matview query / function vs reference SQL on SF1 Postgres.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_mv_correctness.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=mv_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/mv_gate_common.sh"
load_mv_gate_context "${ROOT}" "${QUERY}"
mkdir -p "${GATE_OUT}"

CONTAINER="${ACCEL_PG_CONTAINER:-accel_pg_test}"
DB="${TPCH_PG_DB:-accel_tpch_test}"

if ! ensure_mv_postgres_ready "${ROOT}"; then
  write_mv_gate_json "mv_correctness" "fail" "postgres not ready" "${QUERY}"
  exit 1
fi

# Ensure latest SQL is applied.
set +e
APPLY_LOG="$(bash "${ROOT}/scripts/postgres_matview_apply.sh" "${QUERY}" 2>&1)"
APPLY_RC=$?
set -e
if [[ ${APPLY_RC} -ne 0 ]]; then
  write_mv_gate_json "mv_correctness" "fail" "apply failed before correctness" "${QUERY}"
  echo "${APPLY_LOG}" >&2
  exit 1
fi

REF_SQL="${ROOT}/${MV_REFERENCE_SQL}"
if [[ ! -f "${REF_SQL}" && -n "${REFERENCE_SQL_PATH}" && -f "${REFERENCE_SQL_PATH}" ]]; then
  REF_SQL="${REFERENCE_SQL_PATH}"
fi
QUERY_SQL="${ROOT}/${MV_QUERY_SQL}"
FUNCTION_SQL="${ROOT}/${MV_FUNCTION_SQL}"

if [[ ! -f "${REF_SQL}" ]]; then
  write_mv_gate_json "mv_correctness" "fail" "missing reference SQL (${REF_SQL})" "${QUERY}"
  exit 1
fi

# Prefer LANGUAGE sql function if registered; else run residual query.sql.
COMPARE_MODE="query_sql"
MV_SQL_FILE="${QUERY_SQL}"
if [[ -f "${FUNCTION_SQL}" ]]; then
  # Probe whether function exists.
  set +e
  docker exec -e PGPASSWORD="${PGPASSWORD:-postgres}" "${CONTAINER}" \
    psql -U "${PGUSER:-postgres}" -d "${DB}" -tAc \
    "SELECT 1 FROM pg_proc WHERE proname = '${MV_FUNCTION_NAME##*.}' LIMIT 1;" | grep -q 1
  FN_RC=$?
  set -e
  if [[ ${FN_RC} -eq 0 ]]; then
    COMPARE_MODE="function"
  fi
fi

# Differential via EXCEPT ALL both ways (set equality). For functions, agent must
# also provide query.sql that invokes the function or mirrors residual SELECT.
if [[ ! -f "${QUERY_SQL}" && "${COMPARE_MODE}" != "function" ]]; then
  write_mv_gate_json "mv_correctness" "fail" "missing query.sql for residual path" "${QUERY}"
  exit 1
fi

# Build a temp SQL that checks (ref EXCEPT mv) UNION ALL (mv EXCEPT ref) is empty.
# Reference and residual files must be single SELECT statements (no trailing ; required).
python3 - "${REF_SQL}" "${QUERY_SQL}" "${GATE_OUT}/_mv_diff.sql" <<'PY'
import sys
from pathlib import Path
ref = Path(sys.argv[1]).read_text(encoding="utf-8").strip().rstrip(";")
mv = Path(sys.argv[2]).read_text(encoding="utf-8").strip().rstrip(";")
out = Path(sys.argv[3])
sql = f"""
WITH ref AS (
{ref}
), mv AS (
{mv}
),
only_ref AS (SELECT * FROM ref EXCEPT ALL SELECT * FROM mv),
only_mv AS (SELECT * FROM mv EXCEPT ALL SELECT * FROM ref)
SELECT
  (SELECT COUNT(*) FROM only_ref) AS only_in_reference,
  (SELECT COUNT(*) FROM only_mv) AS only_in_matview;
"""
out.write_text(sql, encoding="utf-8")
PY

set +e
DIFF_OUT="$(docker exec -i \
  -e PGPASSWORD="${PGPASSWORD:-postgres}" \
  "${CONTAINER}" \
  psql -v ON_ERROR_STOP=1 -U "${PGUSER:-postgres}" -d "${DB}" -tA -F ',' \
  < "${GATE_OUT}/_mv_diff.sql" 2>&1)"
DIFF_RC=$?
set -e

if [[ ${DIFF_RC} -ne 0 ]]; then
  write_mv_gate_json "mv_correctness" "fail" "diff query failed: ${DIFF_OUT:0:400}" "${QUERY}"
  echo "${DIFF_OUT}" >&2
  exit 1
fi

ONLY_REF="$(echo "${DIFF_OUT}" | head -1 | cut -d',' -f1 | tr -d '[:space:]')"
ONLY_MV="$(echo "${DIFF_OUT}" | head -1 | cut -d',' -f2 | tr -d '[:space:]')"
if [[ "${ONLY_REF}" == "0" && "${ONLY_MV}" == "0" ]]; then
  write_mv_gate_json "mv_correctness" "pass" "reference matches residual (${COMPARE_MODE})" "${QUERY}"
  exit 0
fi

write_mv_gate_json "mv_correctness" "fail" "mismatch only_ref=${ONLY_REF} only_mv=${ONLY_MV}" "${QUERY}"
echo "mismatch only_ref=${ONLY_REF} only_mv=${ONLY_MV}" >&2
exit 1
