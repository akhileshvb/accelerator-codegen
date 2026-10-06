#!/usr/bin/env bash
# Create TPC-H tables in accel_mysql_test and load the small DuckDB subset.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PYTHON="${ACCEL_PYTHON:-${ROOT}/.venv-pg-test/bin/python}"
if [[ ! -x "$PYTHON" ]] || ! "$PYTHON" -c 'import duckdb' >/dev/null 2>&1; then
  PYTHON="${ROOT}/pipeline/orchestrator/.venv/bin/python3"
fi
if [[ ! -x "$PYTHON" ]] || ! "$PYTHON" -c 'import duckdb' >/dev/null 2>&1; then
  PYTHON=python3
fi
NAME="${ACCEL_MYSQL_CONTAINER:-accel_mysql_test}"
PASSWORD="${MYSQL_PWD:-${MYSQL_ROOT_PASSWORD:-mysql}}"
DB="${TPCH_MYSQL_DB:-accel_mysql_test}"
DUCKDB="${TPCH_TEST_DB:-${ROOT}/../accelerator-codegen-duckdb/tpch_test.duckdb}"

mysql_exec() {
  docker exec -i "$NAME" mysql -h 127.0.0.1 -P 3306 -uroot -p"${PASSWORD}" "$@"
}

if ! docker ps --format '{{.Names}}' | grep -qx "$NAME"; then
  echo "container ${NAME} not running; start with scripts/start_accel_mysql_test.sh" >&2
  exit 1
fi

echo "=== building small TPC-H DuckDB test data ==="
if [[ -f "$DUCKDB" && "${TPCH_FORCE_REBUILD:-0}" != "1" ]]; then
  echo "reusing existing $DUCKDB"
else
  "${PYTHON}" "${ROOT}/scripts/create_tpch_test_db.py" --dest "$DUCKDB" --force
fi

echo "=== recreating TPC-H tables in ${DB} ==="
mysql_exec -e "CREATE DATABASE IF NOT EXISTS \`${DB}\`;"
for t in lineitem orders customer part partsupp supplier nation region; do
  mysql_exec "$DB" -e "DROP TABLE IF EXISTS ${t};"
done
mysql_exec "$DB" < "${ROOT}/scripts/mysql_tpch_schema.sql"

echo "=== loading TPC-H test data from DuckDB into MySQL ==="
ACCEL_MYSQL_DOCKER=1 ACCEL_MYSQL_CONTAINER="$NAME" MYSQL_PWD="$PASSWORD" \
  "${PYTHON}" "${ROOT}/scripts/load_tpch_test_mysql.py" --duckdb "$DUCKDB" --db "$DB"

echo "=== registering TPC-H accelerator UDFs ==="
REGISTER_SQL="$(mktemp)"
"${ROOT}/scripts/mysql_register_functions.sh" > "${REGISTER_SQL}"
mysql_exec "$DB" < "${REGISTER_SQL}" || true
# INSTALL PLUGIN fails if already present; functions still need (re)creating.
while IFS= read -r line; do
  [[ "$line" == INSTALL* ]] && continue
  [[ -z "$line" || "$line" == --* ]] && continue
  mysql_exec "$DB" -e "$line" || true
done < "${REGISTER_SQL}"
rm -f "${REGISTER_SQL}"

echo "=== creating mutation capture triggers ==="
mysql_exec "$DB" < "${ROOT}/scripts/mysql_accel_mutation_triggers.sql"

echo "=== TPC-H MySQL test database ready: $DB ==="
mysql_exec "$DB" -N -e "SELECT 'lineitem', COUNT(*) FROM lineitem UNION ALL SELECT 'orders', COUNT(*) FROM orders;"
