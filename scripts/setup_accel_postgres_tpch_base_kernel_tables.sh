#!/usr/bin/env bash
# Load missing TPC-H tables (region, supplier, partsupp) and extend part for base-kernel tests.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB="${TPCH_PG_DB:-accel_aux_bench_q10}"
DUCKDB_PATH="${ACCEL_TPCH_DB:-/home/akhilvb/accelerator-codegen-duckdb/tpch_sf1.duckdb}"
PYTHON="${ROOT}/.venv-pg-test/bin/python"
if [[ ! -x "$PYTHON" ]]; then
  PYTHON=python3
fi

export PG_HOST="${PG_HOST:-127.0.0.1}"
export PGPORT="${PG_PORT:-5434}"
export PGPASSWORD="${PGPASSWORD:-postgres}"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

"$PYTHON" - <<'PY' "$DUCKDB_PATH" "$TMP_DIR"
import sys
import duckdb

duckdb_path, out_dir = sys.argv[1], sys.argv[2]
con = duckdb.connect(duckdb_path, read_only=True)

con.execute(
    f"COPY (SELECT * FROM region) TO '{out_dir}/region.csv' (HEADER, DELIMITER '|')"
)
con.execute(
    f"COPY (SELECT * FROM supplier) TO '{out_dir}/supplier.csv' (HEADER, DELIMITER '|')"
)
con.execute(
    f"COPY (SELECT * FROM partsupp) TO '{out_dir}/partsupp.csv' (HEADER, DELIMITER '|')"
)
con.execute(
    f"COPY (SELECT * FROM part) TO '{out_dir}/part_full.csv' (HEADER, DELIMITER '|')"
)
PY

psql_cmd() {
  PGPASSWORD="${PGPASSWORD}" psql -h "$PG_HOST" -p "$PGPORT" -U postgres -d "$DB" -v ON_ERROR_STOP=1 "$@"
}

psql_cmd <<'SQL'
CREATE TABLE IF NOT EXISTS region (
  r_regionkey integer NOT NULL,
  r_name character varying NOT NULL,
  r_comment character varying NOT NULL
);

CREATE TABLE IF NOT EXISTS supplier (
  s_suppkey bigint NOT NULL,
  s_name character varying NOT NULL,
  s_address character varying NOT NULL,
  s_nationkey integer NOT NULL,
  s_phone character varying NOT NULL,
  s_acctbal numeric(15,2) NOT NULL,
  s_comment character varying NOT NULL
);

CREATE TABLE IF NOT EXISTS partsupp (
  ps_partkey bigint NOT NULL,
  ps_suppkey bigint NOT NULL,
  ps_availqty integer NOT NULL,
  ps_supplycost numeric(15,2) NOT NULL,
  ps_comment character varying NOT NULL
);

ALTER TABLE part ADD COLUMN IF NOT EXISTS p_mfgr character varying;
ALTER TABLE part ADD COLUMN IF NOT EXISTS p_brand character varying;
ALTER TABLE part ADD COLUMN IF NOT EXISTS p_size integer;
ALTER TABLE part ADD COLUMN IF NOT EXISTS p_container character varying;
ALTER TABLE part ADD COLUMN IF NOT EXISTS p_name character varying;
ALTER TABLE part ADD COLUMN IF NOT EXISTS p_retailprice numeric(15,2);
ALTER TABLE part ADD COLUMN IF NOT EXISTS p_comment character varying;
SQL

psql_cmd -c "TRUNCATE region, supplier, partsupp;"
psql_cmd -c "\\copy region FROM '${TMP_DIR}/region.csv' WITH (FORMAT csv, DELIMITER '|', HEADER true)"
psql_cmd -c "\\copy supplier FROM '${TMP_DIR}/supplier.csv' WITH (FORMAT csv, DELIMITER '|', HEADER true)"
psql_cmd -c "\\copy partsupp FROM '${TMP_DIR}/partsupp.csv' WITH (FORMAT csv, DELIMITER '|', HEADER true)"

psql_cmd <<SQL
CREATE TEMP TABLE part_full (
  p_partkey bigint,
  p_name character varying,
  p_mfgr character varying,
  p_brand character varying,
  p_type character varying,
  p_size integer,
  p_container character varying,
  p_retailprice numeric(15,2),
  p_comment character varying
);
\\copy part_full FROM '${TMP_DIR}/part_full.csv' WITH (FORMAT csv, DELIMITER '|', HEADER true)
UPDATE part p
SET p_name = f.p_name,
    p_mfgr = f.p_mfgr,
    p_brand = f.p_brand,
    p_type = f.p_type,
    p_size = f.p_size,
    p_container = f.p_container,
    p_retailprice = f.p_retailprice,
    p_comment = f.p_comment
FROM part_full f
WHERE p.p_partkey = f.p_partkey;
SQL

echo "loaded region/supplier/partsupp and extended part in ${DB}"
