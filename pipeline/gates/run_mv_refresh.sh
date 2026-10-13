#!/usr/bin/env bash
# Mutate a dependent base row, REFRESH matview, re-check residual vs reference.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
QUERY="${1:-}"
if [[ -z "${QUERY}" ]]; then
  echo "usage: run_mv_refresh.sh <query_number>" >&2
  exit 1
fi

# shellcheck source=mv_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/mv_gate_common.sh"
load_mv_gate_context "${ROOT}" "${QUERY}"
mkdir -p "${GATE_OUT}"

CONTAINER="${ACCEL_PG_CONTAINER:-accel_pg_test}"
DB="${TPCH_PG_DB:-accel_tpch_test}"

if ! ensure_mv_postgres_ready "${ROOT}"; then
  write_mv_gate_json "mv_refresh" "fail" "postgres not ready" "${QUERY}"
  exit 1
fi

set +e
bash "${ROOT}/scripts/postgres_matview_apply.sh" "${QUERY}" >/tmp/accel_mv_refresh_apply.log 2>&1
APPLY_RC=$?
set -e
if [[ ${APPLY_RC} -ne 0 ]]; then
  write_mv_gate_json "mv_refresh" "fail" "apply failed" "${QUERY}"
  exit 1
fi

# Lightweight invalidating touch on lineitem when present (TPC-H); otherwise skip mutate.
set +e
docker exec -e PGPASSWORD="${PGPASSWORD:-postgres}" "${CONTAINER}" \
  psql -U "${PGUSER:-postgres}" -d "${DB}" -v ON_ERROR_STOP=1 -c \
  "UPDATE lineitem SET l_comment = l_comment WHERE l_orderkey = (SELECT l_orderkey FROM lineitem LIMIT 1);" \
  >/tmp/accel_mv_mutate.log 2>&1
MUT_RC=$?
set -e
if [[ ${MUT_RC} -ne 0 ]]; then
  # Table may not exist for non-TPC-H; still exercise REFRESH.
  echo "note: skip base mutate (${MUT_RC}); continuing with REFRESH" >&2
fi

set +e
REFRESH_LOG="$(bash "${ROOT}/scripts/postgres_matview_refresh.sh" "${QUERY}" 2>&1)"
REFRESH_RC=$?
set -e
if [[ ${REFRESH_RC} -ne 0 ]]; then
  write_mv_gate_json "mv_refresh" "fail" "refresh failed: ${REFRESH_LOG:0:300}" "${QUERY}"
  echo "${REFRESH_LOG}" >&2
  exit 1
fi

# Re-use correctness gate logic.
set +e
bash "${ROOT}/pipeline/gates/run_mv_correctness.sh" "${QUERY}" >/tmp/accel_mv_refresh_correct.log 2>&1
CORR_RC=$?
set -e
if [[ ${CORR_RC} -eq 0 ]]; then
  write_mv_gate_json "mv_refresh" "pass" "refresh then correctness ok" "${QUERY}"
  exit 0
fi

write_mv_gate_json "mv_refresh" "fail" "post-refresh correctness failed" "${QUERY}"
tail -40 /tmp/accel_mv_refresh_correct.log >&2 || true
exit 1
