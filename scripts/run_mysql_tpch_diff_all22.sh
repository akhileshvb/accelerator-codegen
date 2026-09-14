#!/usr/bin/env bash
# Run MySQL TPC-H differentials for queries 1..22 inside accel_mysql_test.
set -euo pipefail
KIND="${ACCEL_MYSQL_DIFF_KIND:-aux}"
MODE="${ACCEL_MYSQL_DIFF_MODE:-read}"
SKIP_WM="${ACCEL_MYSQL_DIFF_SKIP_WRITE_MAINT:-1}"
CONTAINER="${ACCEL_MYSQL_CONTAINER:-accel_mysql_test}"
QUERIES="${ACCEL_MYSQL_DIFF_QUERIES:-$(seq 1 22)}"

fail=0
pass=0
for q in $QUERIES; do
  echo "=== Q${q} kind=${KIND} mode=${MODE} skip_wm=${SKIP_WM} ==="
  set +e
  out=$(docker exec \
    -e ACCEL_MYSQL_DOCKER=1 \
    -e MYSQL_HOST=127.0.0.1 -e MYSQL_PORT=3306 \
    -e MYSQL_USER=root -e MYSQL_PWD="${MYSQL_PWD:-mysql}" \
    -e TPCH_MYSQL_DB="${TPCH_MYSQL_DB:-accel_mysql_test}" \
    -e ACCEL_ENGINE_NAME=mysql \
    -e ACCEL_WORKLOAD_KIND=aux_state \
    -e ACCEL_SRC=/accel-src \
    -e ACCELERATOR_CODEGEN_CONFIG_DIR=/accel-src/config \
    -e ACCEL_MYSQL_DIFF_KIND="${KIND}" \
    -e ACCEL_MYSQL_DIFF_MODE="${MODE}" \
    -e ACCEL_MYSQL_DIFF_SKIP_WRITE_MAINT="${SKIP_WM}" \
    -e ACCEL_MYSQL_DIFF_SKIP_MAINT_STRATS="${ACCEL_MYSQL_DIFF_SKIP_MAINT_STRATS:-}" \
    -e ACCEL_MYSQL_DIFF_MAINT_STRATS="${ACCEL_MYSQL_DIFF_MAINT_STRATS:-}" \
    -e ACCEL_FORCE_MAINTENANCE_ACTION="${ACCEL_FORCE_MAINTENANCE_ACTION:-}" \
    -e LD_LIBRARY_PATH=/accel-build/accel:/accel-build/duckdb/src \
    "${CONTAINER}" \
    /accel-build/accel/test_mysql_tpch_differential "${q}" 2>&1)
  rc=$?
  set -e
  echo "$out" | tail -12
  if [[ $rc -eq 0 ]] && echo "$out" | grep -qE 'PASS (read|write_maint)'; then
    pass=$((pass + 1))
  else
    echo "FAIL Q${q} rc=${rc}"
    echo "$out" | tail -40
    fail=$((fail + 1))
  fi
done
echo "summary pass=${pass} fail=${fail}"
exit "${fail}"
