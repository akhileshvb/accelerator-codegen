#!/usr/bin/env bash
# Load tpchgen-cli Parquet output into a DuckDB file with the repo's TPC-H DDL (the schema of tpch_sf1/sf20.duckdb),
# keeping dbgen's row order: part files are inserted in numeric order (a glob would put lineitem.10 before lineitem.2).
#   load_tpch_parquet.sh <parquet_dir> <dest.duckdb>
# parquet_dir: tpchgen-cli -o output, either <dir>/<table>.parquet or <dir>/<table>/<table>.<k>.parquet (--parts).
# Env: DUCKDB_BIN (the fork that runs the accelerators), MEMORY_LIMIT (default 75% of RAM), THREADS (default nproc),
#      TEMP_DIR (spill directory, default <dest>.tmp; must be on disk, not tmpfs).
set -euo pipefail
src=$(cd "$1" && pwd); dest=$2
bin=${DUCKDB_BIN:-duckdb}
mem=${MEMORY_LIMIT:-$(( $(awk '/MemTotal/{print $2}' /proc/meminfo) * 3 / 4 / 1048576 ))GB}
threads=${THREADS:-$(nproc)}
tmp=${TEMP_DIR:-$dest.tmp}
[ -e "$dest" ] && { echo "$dest exists; remove it first" >&2; exit 1; }

files() {  # the table's part files in numeric order
  if [ -f "$src/$1.parquet" ]; then echo "$src/$1.parquet"
  else ls "$src/$1"/"$1".*.parquet | sort -t. -k2,2n; fi
}

{
  echo "SET memory_limit='$mem'; SET threads=$threads; SET temp_directory='$tmp'; SET preserve_insertion_order=true;"
  cat <<'SQL'
CREATE TABLE customer(c_custkey BIGINT NOT NULL, c_name VARCHAR NOT NULL, c_address VARCHAR NOT NULL, c_nationkey INTEGER NOT NULL, c_phone VARCHAR NOT NULL, c_acctbal DECIMAL(15,2) NOT NULL, c_mktsegment VARCHAR NOT NULL, c_comment VARCHAR NOT NULL);
CREATE TABLE lineitem(l_orderkey BIGINT NOT NULL, l_partkey BIGINT NOT NULL, l_suppkey BIGINT NOT NULL, l_linenumber BIGINT NOT NULL, l_quantity DECIMAL(15,2) NOT NULL, l_extendedprice DECIMAL(15,2) NOT NULL, l_discount DECIMAL(15,2) NOT NULL, l_tax DECIMAL(15,2) NOT NULL, l_returnflag VARCHAR NOT NULL, l_linestatus VARCHAR NOT NULL, l_shipdate DATE NOT NULL, l_commitdate DATE NOT NULL, l_receiptdate DATE NOT NULL, l_shipinstruct VARCHAR NOT NULL, l_shipmode VARCHAR NOT NULL, l_comment VARCHAR NOT NULL);
CREATE TABLE nation(n_nationkey INTEGER NOT NULL, n_name VARCHAR NOT NULL, n_regionkey INTEGER NOT NULL, n_comment VARCHAR NOT NULL);
CREATE TABLE orders(o_orderkey BIGINT NOT NULL, o_custkey BIGINT NOT NULL, o_orderstatus VARCHAR NOT NULL, o_totalprice DECIMAL(15,2) NOT NULL, o_orderdate DATE NOT NULL, o_orderpriority VARCHAR NOT NULL, o_clerk VARCHAR NOT NULL, o_shippriority INTEGER NOT NULL, o_comment VARCHAR NOT NULL);
CREATE TABLE part(p_partkey BIGINT NOT NULL, p_name VARCHAR NOT NULL, p_mfgr VARCHAR NOT NULL, p_brand VARCHAR NOT NULL, p_type VARCHAR NOT NULL, p_size INTEGER NOT NULL, p_container VARCHAR NOT NULL, p_retailprice DECIMAL(15,2) NOT NULL, p_comment VARCHAR NOT NULL);
CREATE TABLE partsupp(ps_partkey BIGINT NOT NULL, ps_suppkey BIGINT NOT NULL, ps_availqty BIGINT NOT NULL, ps_supplycost DECIMAL(15,2) NOT NULL, ps_comment VARCHAR NOT NULL);
CREATE TABLE region(r_regionkey INTEGER NOT NULL, r_name VARCHAR NOT NULL, r_comment VARCHAR NOT NULL);
CREATE TABLE supplier(s_suppkey BIGINT NOT NULL, s_name VARCHAR NOT NULL, s_address VARCHAR NOT NULL, s_nationkey INTEGER NOT NULL, s_phone VARCHAR NOT NULL, s_acctbal DECIMAL(15,2) NOT NULL, s_comment VARCHAR NOT NULL);
SQL
  for t in region nation supplier customer part partsupp orders lineitem; do
    for f in $(files "$t"); do
      echo ".print $t $(basename "$f")"
      echo "INSERT INTO $t SELECT * FROM read_parquet('$f');"  # by position; casts to the DDL types
    done
  done
  echo "CHECKPOINT;"
} | "$bin" "$dest"
rm -rf "$tmp"
"$bin" -readonly "$dest" -c "SELECT (SELECT count(*) FROM lineitem) lineitem, (SELECT count(*) FROM orders) orders,
  (SELECT count(*) FROM partsupp) partsupp, (SELECT count(*) FROM part) part, (SELECT count(*) FROM customer) customer,
  (SELECT count(*) FROM supplier) supplier"
ls -lh "$dest"
