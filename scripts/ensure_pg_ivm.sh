#!/usr/bin/env bash
# Install pg_ivm into accel_pg_test and CREATE EXTENSION in a database.
# Usage: bash scripts/ensure_pg_ivm.sh [dbname]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NAME="${ACCEL_PG_CONTAINER:-accel_pg_test}"
DB="${1:-${TPCH_PG_DB:-accel_tpch_sf1}}"
PORT="${PG_PORT:-${PGPORT:-5434}}"

export PGPASSWORD="${PGPASSWORD:-postgres}"
export PGHOST="${PG_HOST:-127.0.0.1}"
export PGPORT="${PORT}"
export PGUSER="${PGUSER:-postgres}"

if ! docker ps --format '{{.Names}}' | grep -qx "${NAME}"; then
  bash "${ROOT}/scripts/ensure_accel_pg_test.sh" || true
fi

if ! docker exec "${NAME}" bash -lc 'test -f /usr/share/postgresql/17/extension/pg_ivm.control'; then
  echo "[pg_ivm] installing postgresql-17-pg-ivm in ${NAME}"
  docker exec -u root "${NAME}" bash -lc \
    'apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq postgresql-17-pg-ivm'
fi

psql -d "${DB}" -v ON_ERROR_STOP=1 -c "CREATE EXTENSION IF NOT EXISTS pg_ivm;"
echo "[pg_ivm] ready in ${DB}"
