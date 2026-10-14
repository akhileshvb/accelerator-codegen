#!/usr/bin/env bash
# Parallel TPC-H index build for SF3000 Postgres.
#
# Replaces the serial postgres_tpch_indexes.sql. Key points:
#  - ALTER TABLE ADD PRIMARY KEY takes ACCESS EXCLUSIVE, which would serialize
#    against every other index build on the same table. So build the PK as a
#    plain UNIQUE INDEX (ShareLock, concurrent-friendly) and attach it afterwards
#    with ADD PRIMARY KEY USING INDEX (instant; columns are already NOT NULL).
#  - idx_lineitem_orderkey is skipped: the lineitem PK (l_orderkey, l_linenumber)
#    already serves l_orderkey lookups.
#  - Small tables (region..part) already have PKs from the first run.
#
# Usage: bash scripts/postgres_tpch_indexes_parallel_sf3000.sh
# Env:   TPCH_PG_DB (accel_tpch_sf3000), IDX_MWM (12GB), IDX_WORKERS (2)
#        LOG_DIR (eval/bench/tpch/results_pg_sf3000/index_build)
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB="${TPCH_PG_DB:-accel_tpch_sf3000}"
MWM="${IDX_MWM:-12GB}"
WORKERS="${IDX_WORKERS:-2}"
LOG_DIR="${LOG_DIR:-${ROOT}/eval/bench/tpch/results_pg_sf3000/index_build}"
export PGPASSWORD="${PGPASSWORD:-postgres}"
export PGHOST="${PG_HOST:-127.0.0.1}"
export PGPORT="${PG_PORT:-${PGPORT:-5434}}"
export PGUSER="${PGUSER:-postgres}"
mkdir -p "${LOG_DIR}"

# Fewer checkpoints during the build (reloadable, no restart).
psql -d postgres -qc "ALTER SYSTEM SET max_wal_size='200GB'" -c "ALTER SYSTEM SET checkpoint_timeout='30min'" -c "SELECT pg_reload_conf()" >/dev/null

# run <name> <sql...>: one psql session, statements run in order, timed.
run() {
  local name="$1"; shift
  local sql=""
  for s in "$@"; do sql+="${s};"$'\n'; done
  (
    start=$(date +%s)
    echo "[$(date +%T)] START ${name}"
    psql -d "${DB}" -v ON_ERROR_STOP=1 -qtA <<SQL
SET maintenance_work_mem='${MWM}';
SET max_parallel_maintenance_workers=${WORKERS};
\timing on
${sql}
SQL
    rc=$?
    echo "[$(date +%T)] END ${name} rc=${rc} elapsed=$(( $(date +%s) - start ))s"
  ) >"${LOG_DIR}/${name}.log" 2>&1 &
}

# --- lineitem: all ShareLock builds run concurrently (shared seq scans help) ---
run lineitem_pk_idx  "CREATE UNIQUE INDEX lineitem_pkey ON lineitem (l_orderkey, l_linenumber)"
run lineitem_partkey "CREATE INDEX idx_lineitem_partkey ON lineitem (l_partkey)"
run lineitem_suppkey "CREATE INDEX idx_lineitem_suppkey ON lineitem (l_suppkey)"
run lineitem_shipdate "CREATE INDEX idx_lineitem_shipdate ON lineitem (l_shipdate)"

# --- orders ---
run orders_all \
  "CREATE UNIQUE INDEX orders_pkey ON orders (o_orderkey)" \
  "CREATE INDEX idx_orders_custkey ON orders (o_custkey)" \
  "CREATE INDEX idx_orders_orderdate ON orders (o_orderdate)"

# --- small tables (partsupp PK is still finishing in the original session;
#     wait for it, then add the secondary index) ---
run small_tables \
  "CREATE INDEX idx_nation_regionkey ON nation (n_regionkey)" \
  "CREATE INDEX idx_customer_nationkey ON customer (c_nationkey)" \
  "CREATE INDEX idx_customer_mktsegment ON customer (c_mktsegment)" \
  "CREATE INDEX idx_supplier_nationkey ON supplier (s_nationkey)" \
  "CREATE INDEX idx_partsupp_suppkey ON partsupp (ps_suppkey)"   # blocks until partsupp PK commits

wait

# Attach PKs (metadata-only once the unique index exists and columns are NOT NULL).
echo "[$(date +%T)] attaching primary keys"
psql -d "${DB}" -v ON_ERROR_STOP=1 <<'SQL'
ALTER TABLE lineitem ADD PRIMARY KEY USING INDEX lineitem_pkey;
ALTER TABLE orders   ADD PRIMARY KEY USING INDEX orders_pkey;
SQL

echo "[$(date +%T)] ANALYZE"
psql -d "${DB}" -c "ANALYZE"
echo "[$(date +%T)] DONE"
grep -H "END " "${LOG_DIR}"/*.log
