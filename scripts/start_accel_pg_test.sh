#!/usr/bin/env bash
# Start/recreate accel_pg_test (Postgres 17) with source mounts for extension builds.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DUCKDB_ROOT="${DUCKDB_ROOT:-${ROOT}/../accelerator-codegen-duckdb}"
DATA_DIR="${ACCEL_PG_DATA:-/disk3/akhilvb/accel_pg_test_data}"
BUILD_DIR="${ACCEL_PG_BUILD:-/disk3/akhilvb/accel_pg_build}"
NAME="${ACCEL_PG_CONTAINER:-accel_pg_test}"
PORT="${PG_PORT:-5434}"

mkdir -p "$DATA_DIR" "$BUILD_DIR"

if docker ps -a --format '{{.Names}}' | grep -qx "$NAME"; then
  echo "=== removing existing ${NAME} ==="
  docker rm -f "$NAME" >/dev/null
fi

echo "=== starting ${NAME} on 127.0.0.1:${PORT} ==="
docker run -d --name "$NAME" \
  --shm-size=8g \
  -e POSTGRES_PASSWORD="${PGPASSWORD:-postgres}" \
  -e ACCELERATOR_CODEGEN_CONFIG_DIR=/accel-src/config \
  -p "127.0.0.1:${PORT}:5432" \
  -v "${DATA_DIR}:/var/lib/postgresql/data" \
  -v "${ROOT}:/accel-src" \
  -v "${DUCKDB_ROOT}:/accel-duckdb:ro" \
  -v "${BUILD_DIR}:/accel-build" \
  postgres:17 \
  -c shared_buffers=32GB \
  -c effective_cache_size=256GB \
  -c work_mem=256MB \
  -c maintenance_work_mem=4GB \
  -c wal_buffers=64MB \
  -c max_wal_size=16GB \
  -c checkpoint_completion_target=0.9 \
  -c random_page_cost=1.1 \
  -c effective_io_concurrency=200 \
  -c autovacuum=off \
  >/dev/null

for _ in $(seq 1 60); do
  if PGPASSWORD="${PGPASSWORD:-postgres}" psql -h 127.0.0.1 -p "$PORT" -U postgres -d postgres -tAc 'select 1' >/dev/null 2>&1; then
    echo "PostgreSQL ready on 127.0.0.1:${PORT}"
    exit 0
  fi
  sleep 1
done
echo "timed out waiting for ${NAME}" >&2
exit 1
