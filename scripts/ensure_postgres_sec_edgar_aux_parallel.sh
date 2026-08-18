#!/usr/bin/env bash
# Install accel_sec_edgar_q*_aux_parallel() into an existing Postgres DB.
# Rebuilds the docker extension .so with ACCEL_ENABLE_PARALLEL_AUX=ON.
# Does not DROP ST accel_*_aux functions or reload SF3 data.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PG_HOST="${PG_HOST:-127.0.0.1}"
PG_PORT="${PG_PORT:-5434}"
PG_SEC_DB="${PG_SEC_DB:-accel_sec_edgar_sf3}"
CONTAINER="${ACCEL_PG_CONTAINER:-accel_pg_test}"
EXTENSION_SO="${ACCEL_POSTGRES_SO:-/usr/lib/postgresql/17/lib/accelerator_codegen_postgres.so}"
PARALLEL_SQL="${ROOT}/src/runtime/postgres/accelerator_codegen_postgres_aux_parallel.sql"

export PGHOST="$PG_HOST"
export PGPORT="$PG_PORT"
export PGUSER="${PG_USER:-postgres}"
export PGPASSWORD="${PGPASSWORD:-postgres}"

psql_cmd() {
  psql -v ON_ERROR_STOP=1 "$@"
}

if ! psql_cmd -d postgres -tAc 'select 1' >/dev/null 2>&1; then
  echo "PostgreSQL is not reachable at ${PG_HOST}:${PG_PORT}" >&2
  exit 1
fi

if docker ps --format '{{.Names}}' | grep -qx "$CONTAINER"; then
  echo "=== syncing parallel-aux sources into ${CONTAINER} ==="
  docker cp "${ROOT}/CMakeLists.txt" "${CONTAINER}:/tmp/accel-codegen-src/CMakeLists.txt"
  docker cp "${ROOT}/cmake/." "${CONTAINER}:/tmp/accel-codegen-src/cmake/"
  docker cp "${ROOT}/src/generated/." "${CONTAINER}:/tmp/accel-codegen-src/src/generated/"
  docker cp "${ROOT}/src/runtime/." "${CONTAINER}:/tmp/accel-codegen-src/src/runtime/"
  docker cp "${ROOT}/src/stateful/." "${CONTAINER}:/tmp/accel-codegen-src/src/stateful/"
  docker cp "${ROOT}/include/accelerator_codegen/." \
    "${CONTAINER}:/tmp/accel-codegen-src/include/accelerator_codegen/"
  # CMake configure enumerates test/bench targets even when we only build the
  # postgres .so; keep those sources present in the container tree.
  docker cp "${ROOT}/tests/." "${CONTAINER}:/tmp/accel-codegen-src/tests/"
  docker cp "${ROOT}/benchmarks/." "${CONTAINER}:/tmp/accel-codegen-src/benchmarks/"
  docker exec "$CONTAINER" bash -c '
    set -e
    cd /tmp/accel-codegen-src/build
    cmake -S /tmp/accel-codegen-src -B /tmp/accel-codegen-src/build \
      -DACCEL_ENABLE_PARALLEL_AUX=ON \
      -DACCELERATOR_CODEGEN_BUILD_POSTGRES=ON
    cmake --build . --target accelerator_codegen_postgres -j"$(nproc)"
    cp -f accelerator_codegen_postgres.so /usr/lib/postgresql/17/lib/
    nm -D /usr/lib/postgresql/17/lib/accelerator_codegen_postgres.so \
      | grep -q AcceleratorCodegenAuxSecEdgarQ2Parallel
  '
  EXTENSION_SO="/usr/lib/postgresql/17/lib/accelerator_codegen_postgres.so"
else
  echo "warning: ${CONTAINER} is not running; installing SQL against ${EXTENSION_SO}" >&2
fi

echo "=== installing *_aux_parallel SQL on ${PG_SEC_DB} ==="
sed "s|MODULE_PATHNAME|${EXTENSION_SO}|g" "$PARALLEL_SQL" | psql_cmd -d "$PG_SEC_DB" -f -

psql_cmd -d "$PG_SEC_DB" -c \
  "SELECT proname FROM pg_proc WHERE proname LIKE 'accel_sec_edgar%aux%' ORDER BY 1;"
echo "=== parallel aux functions ready on ${PG_SEC_DB} ==="
