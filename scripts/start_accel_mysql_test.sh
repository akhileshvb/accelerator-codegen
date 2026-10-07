#!/usr/bin/env bash
# Start/recreate accel_mysql_test (MySQL 8.4) with source mounts for plugin builds.
# ACCEL_MYSQL_EXTRA_ARGS: more mysqld arguments (e.g. "--innodb-buffer-pool-size=24G").  ACCEL_MYSQL_VOLUME may be a
# host path (bind mount), to keep the data off the Docker volume disk.
# Defaults: portable cache under $HOME/.cache/accel_mysql (override with ACCEL_MYSQL_DATA / ACCEL_MYSQL_BUILD).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DUCKDB_ROOT="${DUCKDB_ROOT:-${ACCEL_DUCKDB_ROOT:-${ROOT}/../accelerator-codegen-duckdb}}"
CACHE_ROOT="${ACCEL_MYSQL_CACHE:-${HOME}/.cache/accel_mysql}"
NAME="${ACCEL_MYSQL_CONTAINER:-accel_mysql_test}"
VOLUME_NAME="${ACCEL_MYSQL_VOLUME:-${NAME}_data}"
BUILD_DIR="${ACCEL_MYSQL_BUILD:-${CACHE_ROOT}/build}"
PORT="${MYSQL_TCP_PORT:-${MYSQL_PORT:-3307}}"
IMAGE="${ACCEL_MYSQL_IMAGE:-mysql:8.4}"
PASSWORD="${MYSQL_PWD:-${MYSQL_ROOT_PASSWORD:-mysql}}"
DATABASE="${MYSQL_DATABASE:-accel_mysql_test}"

if [[ ! -d "${DUCKDB_ROOT}" ]]; then
  echo "DuckDB sibling missing: ${DUCKDB_ROOT}" >&2
  echo "Set DUCKDB_ROOT / ACCEL_DUCKDB_ROOT" >&2
  exit 1
fi

mkdir -p "$BUILD_DIR"

if docker ps -a --format '{{.Names}}' | grep -qx "$NAME"; then
  echo "=== removing existing ${NAME} ==="
  docker rm -f "$NAME" >/dev/null
fi

echo "=== starting ${NAME} (${IMAGE}) on 127.0.0.1:${PORT} ==="
docker run -d --name "$NAME" \
  --shm-size="${ACCEL_MYSQL_SHM_SIZE:-2g}" \
  -e MYSQL_ROOT_PASSWORD="${PASSWORD}" \
  -e MYSQL_DATABASE="${DATABASE}" \
  -e ACCELERATOR_CODEGEN_CONFIG_DIR=/accel-src/config \
  -p "127.0.0.1:${PORT}:3306" \
  -v "${VOLUME_NAME}:/var/lib/mysql" \
  -v "${ROOT}:/accel-src" \
  -v "${DUCKDB_ROOT}:/accel-duckdb:ro" \
  -v "${BUILD_DIR}:/accel-build" \
  "${IMAGE}" \
  --local-infile=1 ${ACCEL_MYSQL_EXTRA_ARGS:-} \
  >/dev/null

for _ in $(seq 1 120); do
  if docker exec "$NAME" mysqladmin ping -h 127.0.0.1 -uroot -p"${PASSWORD}" --silent >/dev/null 2>&1; then
    echo "MySQL ready on 127.0.0.1:${PORT} (container=${NAME})"
    exit 0
  fi
  sleep 1
done
echo "timed out waiting for ${NAME}" >&2
docker logs "$NAME" 2>&1 | tail -40 >&2 || true
exit 1
