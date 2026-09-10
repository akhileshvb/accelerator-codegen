#!/usr/bin/env bash
# Run Postgres TPC-H differentials for queries 1..22 inside accel_pg_test.
set -euo pipefail
KIND="${ACCEL_PG_DIFF_KIND:-aux}"
MODE="${ACCEL_PG_DIFF_MODE:-read}"
SKIP_WM="${ACCEL_PG_DIFF_SKIP_WRITE_MAINT:-1}"
CONTAINER="${ACCEL_PG_CONTAINER:-accel_pg_test}"
QUERIES="${ACCEL_PG_DIFF_QUERIES:-$(seq 1 22)}"

fail=0
pass=0
for q in $QUERIES; do
  echo "=== Q${q} kind=${KIND} mode=${MODE} ==="
  set +e
  out=$(docker exec \
    -e ACCEL_PG_DOCKER=1 \
    -e PGHOST=127.0.0.1 -e PGPORT=5432 \
    -e PGUSER="${PGUSER:-postgres}" -e PGPASSWORD="${PGPASSWORD:-postgres}" \
    -e TPCH_PG_DB="${TPCH_PG_DB:-accel_tpch_test}" \
    -e ACCEL_PG_DIFF_KIND="${KIND}" \
    -e ACCEL_PG_DIFF_MODE="${MODE}" \
    -e ACCEL_PG_DIFF_SKIP_WRITE_MAINT="${SKIP_WM}" \
    -e LD_LIBRARY_PATH=/accel-build/accel:/accel-build/duckdb/src:/usr/lib/postgresql/17/lib \
    "${CONTAINER}" \
    /accel-build/accel/test_postgres_tpch_differential "${q}" 2>&1)
  rc=$?
  set -e
  echo "$out" | tail -6
  if [[ $rc -eq 0 ]] && echo "$out" | grep -qE 'PASS (read|write_maint)'; then
    pass=$((pass + 1))
  else
    echo "FAIL Q${q} rc=${rc}"
    fail=$((fail + 1))
  fi
done
echo "summary pass=${pass} fail=${fail}"
exit "${fail}"
