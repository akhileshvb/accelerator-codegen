#!/usr/bin/env bash
# Start/recreate accel_pg_test (Postgres 17) with source mounts for extension builds.
# ACCEL_PG_EXTRA_ARGS: more server arguments (e.g. "-c maintenance_work_mem=4GB"), after the defaults so they win.
# Defaults: portable cache under $HOME/.cache/accel_pg (override with ACCEL_PG_DATA / ACCEL_PG_BUILD).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DUCKDB_ROOT="${DUCKDB_ROOT:-${ACCEL_DUCKDB_ROOT:-${ROOT}/../accelerator-codegen-duckdb}}"
CACHE_ROOT="${ACCEL_PG_CACHE:-${HOME}/.cache/accel_pg}"
DATA_DIR="${ACCEL_PG_DATA:-${CACHE_ROOT}/data}"
BUILD_DIR="${ACCEL_PG_BUILD:-${CACHE_ROOT}/build}"
NAME="${ACCEL_PG_CONTAINER:-accel_pg_test}"
PORT="${PG_PORT:-${PGPORT:-5434}}"
IMAGE="${ACCEL_PG_IMAGE:-postgres:17}"

if [[ ! -d "${DUCKDB_ROOT}" ]]; then
  echo "DuckDB sibling missing: ${DUCKDB_ROOT}" >&2
  echo "Set DUCKDB_ROOT / ACCEL_DUCKDB_ROOT" >&2
  exit 1
fi

mkdir -p "$DATA_DIR" "$BUILD_DIR"

if docker ps -a --format '{{.Names}}' | grep -qx "$NAME"; then
  echo "=== removing existing ${NAME} ==="
  docker rm -f "$NAME" >/dev/null
fi

echo "=== starting ${NAME} (${IMAGE}) on 127.0.0.1:${PORT} ==="
docker run -d --name "$NAME" \
  --shm-size="${ACCEL_PG_SHM_SIZE:-2g}" \
  -e POSTGRES_PASSWORD="${PGPASSWORD:-postgres}" \
  -p "127.0.0.1:${PORT}:5432" \
  -v "${DATA_DIR}:/var/lib/postgresql/data" \
  -v "${ROOT}:/accel-src" \
  -v "${DUCKDB_ROOT}:/accel-duckdb:ro" \
  -v "${BUILD_DIR}:/accel-build" \
  "${IMAGE}" \
  -c shared_buffers="${ACCEL_PG_SHARED_BUFFERS:-256MB}" \
  -c work_mem=64MB \
  -c maintenance_work_mem=256MB \
  -c max_wal_size=2GB \
  -c autovacuum=off \
  ${ACCEL_PG_EXTRA_ARGS:-} \
  >/dev/null

for _ in $(seq 1 90); do
  if PGPASSWORD="${PGPASSWORD:-postgres}" psql -h 127.0.0.1 -p "$PORT" -U postgres -d postgres -tAc 'select 1' >/dev/null 2>&1; then
    echo "PostgreSQL ready on 127.0.0.1:${PORT} (container=${NAME})"
    exit 0
  fi
  sleep 1
done
echo "timed out waiting for ${NAME}" >&2
docker logs "$NAME" 2>&1 | tail -40 >&2 || true
exit 1
