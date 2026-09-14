#!/usr/bin/env bash
# Multi-conn + cross-accel for TPC-H Q1-22 inside accel_pg_test.
set -euo pipefail
CONTAINER="${ACCEL_PG_CONTAINER:-accel_pg_test}"
QUERIES="${ACCEL_PG_DIFF_QUERIES:-$(seq 1 22)}"
fail=0
pass=0

run_one() {
  local bin="$1"
  local q="$2"
  shift 2
  set +e
  out=$(docker exec \
    -e ACCEL_PG_DOCKER=1 -e PGHOST=127.0.0.1 -e PGPORT=5432 \
    -e PGUSER="${PGUSER:-postgres}" -e PGPASSWORD="${PGPASSWORD:-postgres}" \
    -e TPCH_PG_DB="${TPCH_PG_DB:-accel_tpch_test}" \
    -e ACCEL_ENGINE_NAME=postgres -e ACCEL_WORKLOAD_KIND=aux_state \
    -e ACCEL_PG_DIFF_KIND=aux -e ACCEL_SRC=/accel-src \
    -e LD_LIBRARY_PATH=/accel-build/accel:/accel-build/duckdb/src:/usr/lib/postgresql/17/lib \
    "$@" \
    "${CONTAINER}" "/accel-build/accel/${bin}" 2>&1)
  rc=$?
  set -e
  echo "$out" | tail -8
  if [[ $rc -eq 0 ]]; then
    pass=$((pass + 1))
  else
    echo "FAIL ${bin} Q${q} rc=${rc}"
    echo "$out" | tail -30
    fail=$((fail + 1))
  fi
}

echo "===== multi-connection ====="
for q in $QUERIES; do
  echo "=== multi-conn Q${q} ==="
  run_one test_postgres_aux_multi_connection_stress "$q" \
    -e ACCEL_MULTI_CONN_TEST_QUERIES="${q}" \
    -e ACCEL_MULTI_CONN_TARGET_READERS=1 \
    -e ACCEL_MULTI_CONN_BACKGROUND_READERS=1 \
    -e ACCEL_MULTI_CONN_STRESS_SEC="${ACCEL_MULTI_CONN_STRESS_SEC:-6}"
done

echo "===== cross-accel ====="
for q in $QUERIES; do
  echo "=== cross-accel Q${q} ==="
  run_one test_postgres_cross_accel "$q" \
    -e ACCEL_CROSS_ACCEL_TEST_QUERIES="${q}"
done

echo "summary pass=${pass} fail=${fail}"
exit "${fail}"
