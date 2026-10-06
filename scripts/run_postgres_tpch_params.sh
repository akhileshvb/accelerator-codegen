#!/usr/bin/env bash
# Run the TPC-H parameter-generality differential (test_postgres_tpch_params)
# inside a Postgres container that has the accelerator extension installed.
#
#   ACCEL_PG_CONTAINER=accel_pg_test scripts/run_postgres_tpch_params.sh [queries] [draws] [seed] [flags...]
#
# Env: ACCEL_PG_CONTAINER (default accel_pg_test), TPCH_PG_DB (default accel_tpch_test),
#      ACCEL_PG_PARAMS_MODES (default "plain perturb wide": which runs to do; each is a
#      separate invocation: plain, --data-perturb, --wide-keys).
# Extra flags (--keep-going, --max-edge=N, --timeout-ms=N, --classes=abc, ...) are passed through.
set -euo pipefail
CONTAINER="${ACCEL_PG_CONTAINER:-accel_pg_test}"
QUERIES="${1:-all}"
DRAWS="${2:-10}"
SEED="${3:-1}"
shift $(( $# > 3 ? 3 : $# )) || true
MODES="${ACCEL_PG_PARAMS_MODES:-plain perturb wide}"

fail=0
for mode in $MODES; do
  case "$mode" in
    plain) flags=(--plain) ;;
    perturb) flags=(--data-perturb) ;;
    wide) flags=(--wide-keys) ;;
    *) echo "unknown mode $mode" >&2; exit 2 ;;
  esac
  echo "=== test_postgres_tpch_params mode=${mode} queries=${QUERIES} draws=${DRAWS} seed=${SEED} ==="
  set +e
  docker exec \
    -e ACCEL_PG_DOCKER=1 \
    -e PGHOST=127.0.0.1 -e PGPORT=5432 \
    -e PGUSER="${PGUSER:-postgres}" -e PGPASSWORD="${PGPASSWORD:-postgres}" \
    -e TPCH_PG_DB="${TPCH_PG_DB:-accel_tpch_test}" \
    -e ACCEL_ENGINE_NAME=postgres \
    -e ACCEL_WORKLOAD_KIND=aux_state \
    -e ACCEL_SRC=/accel-src \
    -e ACCEL_PG_DIFF_KIND=aux \
    -e LD_LIBRARY_PATH=/accel-build/accel:/accel-build/duckdb/src:/usr/lib/postgresql/17/lib \
    "${CONTAINER}" \
    /accel-build/accel/test_postgres_tpch_params "${QUERIES}" "${DRAWS}" "${SEED}" "${flags[@]}" "$@"
  rc=$?
  set -e
  [[ $rc -eq 0 ]] || fail=$((fail + 1))
done
echo "params summary: modes_failed=${fail}"
exit "${fail}"
