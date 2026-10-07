#!/usr/bin/env bash
# TPC-H SF20 databases for the Postgres and MySQL accelerator experiments
# (benchmarks/compare/run_backends.sh).  Loads from the DuckDB SF20 file.
#
#   bash scripts/setup_sf20_sql_backends.sh export     # DuckDB -> pipe-delimited files in $STAGE (~20 GB)
#   bash scripts/setup_sf20_sql_backends.sh postgres   # database $PG_DB in the Postgres at $PG_PORT
#   bash scripts/setup_sf20_sql_backends.sh mysql      # database $MYSQL_DB in the MySQL container $MYSQL_CONTAINER
#
# NOT run by the benchmark scripts; it is slow (hours) and needs ~60 GB of disk per engine.
#
# Things that decide whether the comparison is fair or even valid:
#  * Postgres: the container from start_accel_pg_test.sh uses shared_buffers=256MB and autovacuum=off.  At SF20
#    give the base DBMS a real cache, keep the data off a small root disk, and build the extension in it:
#      ACCEL_PG_CONTAINER=accel_pg_sf20 PG_PORT=5436 ACCEL_PG_DATA=/big/disk/pg/data \
#      ACCEL_PG_SHARED_BUFFERS=16GB ACCEL_PG_SHM_SIZE=20g \
#      ACCEL_PG_EXTRA_ARGS="-c max_wal_size=32GB -c checkpoint_timeout=30min -c maintenance_work_mem=4GB" \
#        bash scripts/start_accel_pg_test.sh
#      ACCEL_PG_CONTAINER=accel_pg_sf20 bash scripts/build_accel_postgres_extension.sh
#  * MySQL:    innodb_buffer_pool_size defaults to 128MB; set it.  The plugin scans the database named by the server's
#              MYSQL_DATABASE (or mysql_accel_set_scan_database, which run_sql_backend.py calls on every accelerator
#              connection).  The per-query modules follow MySQL's DEFAULT collation (case-insensitive), so this script
#              creates the database with the default collation; do not use a _bin one.
#      ACCEL_MYSQL_CONTAINER=accel_mysql_sf20 MYSQL_TCP_PORT=3320 ACCEL_MYSQL_VOLUME=/big/disk/mysql/data \
#      MYSQL_DATABASE=tpch_sf20 \
#      ACCEL_MYSQL_EXTRA_ARGS="--innodb-buffer-pool-size=24G --innodb-redo-log-capacity=8G --max-allowed-packet=256M" \
#        bash scripts/start_accel_mysql_test.sh
#      ACCEL_MYSQL_CONTAINER=accel_mysql_sf20 TPCH_MYSQL_DB=tpch_sf20 bash scripts/build_accel_mysql_plugin.sh
#  * Both engines get the same indexes (PKs + the usual FK/date indexes); the baseline is not handicapped.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE=${1:?export | postgres | mysql}
SRC=${SRC:-${ROOT}/../accelerator-codegen-duckdb/tpch_sf20.duckdb}
STAGE=${STAGE:-/flash1/akhilvb/tpch/csv_sf20}
DUCKDB_BIN=${DUCKDB_BIN:-$ROOT/../accelerator-codegen-duckdb/build/release/duckdb}
PYTHON=${PYTHON:-python3}
TABLES="region nation customer supplier part partsupp orders lineitem"

PG_HOST=${PG_HOST:-127.0.0.1}; PG_PORT=${PG_PORT:-5434}; PG_DB=${PG_DB:-accel_tpch_sf20}
export PGPASSWORD=${PGPASSWORD:-postgres}
PG_SO=${ACCEL_POSTGRES_SO:-/usr/lib/postgresql/17/lib/accelerator_codegen_pg.so}
MYSQL_CONTAINER=${MYSQL_CONTAINER:-accel_mysql_sf20}
MYSQL_PORT=${MYSQL_TCP_PORT:-3307}; MYSQL_DB=${MYSQL_DB:-tpch_sf20}
MYSQL_PWD=${MYSQL_PWD:-mysql}

psql_() { psql -h "$PG_HOST" -p "$PG_PORT" -U "${PGUSER:-postgres}" -v ON_ERROR_STOP=1 "$@"; }
mysql_() { docker exec -i "$MYSQL_CONTAINER" mysql -uroot -p"$MYSQL_PWD" "$@" 2> >(grep -v 'Using a password' >&2); }

case $MODE in
export)
  [ -x "$DUCKDB_BIN" ] || { echo "no duckdb CLI at $DUCKDB_BIN (set DUCKDB_BIN)" >&2; exit 1; }
  mkdir -p "$STAGE"
  for t in $TABLES; do
    echo "exporting $t"
    "$DUCKDB_BIN" "$SRC" -readonly -c "COPY $t TO '$STAGE/$t.tbl' (FORMAT CSV, DELIMITER '|', HEADER false, NULLSTR '\\N')"
  done
  ls -la "$STAGE"
  ;;
postgres)
  psql_ -d postgres -c "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname='$PG_DB' AND pid<>pg_backend_pid()" >/dev/null
  psql_ -d postgres -c "DROP DATABASE IF EXISTS \"$PG_DB\"" -c "CREATE DATABASE \"$PG_DB\""
  psql_ -d "$PG_DB" -f "$ROOT/scripts/postgres_tpch_schema.sql"        # C-collated text columns
  for t in $TABLES; do
    echo "loading $t $(date +%T)"
    psql_ -d "$PG_DB" -c "\\copy $t FROM '$STAGE/$t.tbl' WITH (FORMAT csv, DELIMITER '|', NULL '\\N')"
  done
  echo "indexes $(date +%T)"
  psql_ -d "$PG_DB" -f "$ROOT/scripts/postgres_tpch_indexes.sql"
  psql_ -d "$PG_DB" -c "LOAD '$PG_SO'" -f "$ROOT/scripts/postgres_register_tpch_all.sql"
  psql_ -d "$PG_DB" -f "$ROOT/scripts/postgres_tpch_mutation_triggers.sql"   # per-table capture triggers (ids 1..8)
  psql_ -d "$PG_DB" -Atc "SELECT 'lineitem', COUNT(*) FROM lineitem" \
    -c "SELECT 'accelerators', COUNT(*) FROM pg_proc WHERE proname LIKE 'accel_tpch_q%_aux'" \
    -c "SELECT 'triggers', COUNT(*) FROM pg_trigger WHERE NOT tgisinternal"
  ;;
mysql)
  have=$(docker exec "$MYSQL_CONTAINER" printenv MYSQL_DATABASE 2>/dev/null || true)
  [ "$have" = "$MYSQL_DB" ] || echo "note: container $MYSQL_CONTAINER has MYSQL_DATABASE='$have' (not '$MYSQL_DB'); the plugin's scan" \
    "database then comes from mysql_accel_set_scan_database, which run_sql_backend.py calls" >&2
  mysql_ -e "DROP DATABASE IF EXISTS \`$MYSQL_DB\`; CREATE DATABASE \`$MYSQL_DB\`"   # default collation (utf8mb4_0900_ai_ci)
  mysql_ "$MYSQL_DB" < "$ROOT/scripts/mysql_tpch_schema.sql"
  "$ROOT/scripts/mysql_register_functions.sh" | grep -v '^--' | grep -v '^$' | while IFS= read -r line; do
    [[ "$line" == INSTALL* ]] && { mysql_ -e "$line" || true; continue; }
    mysql_ "$MYSQL_DB" -e "$line" || true
  done
  # bulk load through the client side (local_infile), skipping the capture triggers (not created yet)
  $PYTHON - <<EOF
import pymysql, time
c = pymysql.connect(host="127.0.0.1", port=$MYSQL_PORT, user="root", password="$MYSQL_PWD", database="$MYSQL_DB",
                    local_infile=True, autocommit=False)
cur = c.cursor()
cur.execute("SET unique_checks=0, foreign_key_checks=0")
for t in "$TABLES".split():
    print("loading", t, time.strftime("%T"), flush=True)
    cur.execute(f"LOAD DATA LOCAL INFILE '$STAGE/{t}.tbl' INTO TABLE {t} FIELDS TERMINATED BY '|' LINES TERMINATED BY '\n'")
    c.commit()
print("indexes", time.strftime("%T"), flush=True)
for ddl in ["CREATE INDEX idx_nation_regionkey ON nation (n_regionkey)", "CREATE INDEX idx_customer_nationkey ON customer (c_nationkey)",
            "CREATE INDEX idx_customer_mktsegment ON customer (c_mktsegment)", "CREATE INDEX idx_supplier_nationkey ON supplier (s_nationkey)",
            "CREATE INDEX idx_partsupp_suppkey ON partsupp (ps_suppkey)", "CREATE INDEX idx_orders_custkey ON orders (o_custkey)",
            "CREATE INDEX idx_orders_orderdate ON orders (o_orderdate)", "CREATE INDEX idx_lineitem_orderkey ON lineitem (l_orderkey)",
            "CREATE INDEX idx_lineitem_partkey ON lineitem (l_partkey)", "CREATE INDEX idx_lineitem_suppkey ON lineitem (l_suppkey)",
            "CREATE INDEX idx_lineitem_shipdate ON lineitem (l_shipdate)"]:
    try:
        cur.execute(ddl)
    except pymysql.err.OperationalError as e:   # 1061: the schema already has it
        if e.args[0] != 1061:
            raise
for t in "$TABLES".split():
    cur.execute(f"ANALYZE TABLE {t}")
    cur.fetchall()
EOF
  mysql_ "$MYSQL_DB" < "$ROOT/scripts/mysql_accel_mutation_triggers.sql"   # triggers only after the load
  mysql_ "$MYSQL_DB" -N -e "SELECT 'lineitem', COUNT(*) FROM lineitem; SELECT 'triggers', COUNT(*) FROM information_schema.triggers WHERE trigger_schema = DATABASE()"
  ;;
*) echo "export | postgres | mysql"; exit 2 ;;
esac
