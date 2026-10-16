#!/usr/bin/env bash
# SF1 copies with SF3000 magnitudes, to catch overflow and key-width bugs at
# small scale:
#   <out>/tpch_shift.duckdb  every key shifted so its maximum equals SF3000's
#   <out>/tpch_big.duckdb    every money column x3000 (SF3000-sized sums)
# Usage: make_tpch_magnitude_dbs.sh <tpch_sf1.duckdb> <out_dir>
set -euo pipefail
src="$1"; out="$2"
duckdb_bin="${DUCKDB_BIN:-duckdb}"
mkdir -p "$out"

cp "$src" "$out/tpch_shift.duckdb"
"$duckdb_bin" "$out/tpch_shift.duckdb" <<'SQL'
-- SF3000 maxima: orderkey 1.8e10 (sparse 4/32 rule), partkey 6e8, suppkey 3e7, custkey 4.5e8.
CREATE TEMP TABLE shift AS SELECT
  18000000000 - (SELECT max(o_orderkey) FROM orders) AS o,
  600000000 - (SELECT max(p_partkey) FROM part) AS p,
  30000000 - (SELECT max(s_suppkey) FROM supplier) AS s,
  450000000 - (SELECT max(c_custkey) FROM customer) AS c;
UPDATE lineitem SET l_orderkey = l_orderkey + (SELECT o FROM shift),
                    l_partkey = l_partkey + (SELECT p FROM shift),
                    l_suppkey = l_suppkey + (SELECT s FROM shift);
UPDATE orders SET o_orderkey = o_orderkey + (SELECT o FROM shift),
                  o_custkey = o_custkey + (SELECT c FROM shift);
UPDATE customer SET c_custkey = c_custkey + (SELECT c FROM shift);
UPDATE part SET p_partkey = p_partkey + (SELECT p FROM shift);
UPDATE partsupp SET ps_partkey = ps_partkey + (SELECT p FROM shift),
                    ps_suppkey = ps_suppkey + (SELECT s FROM shift);
UPDATE supplier SET s_suppkey = s_suppkey + (SELECT s FROM shift);
CHECKPOINT;
SQL

cp "$src" "$out/tpch_big.duckdb"
"$duckdb_bin" "$out/tpch_big.duckdb" <<'SQL'
UPDATE lineitem SET l_extendedprice = l_extendedprice * 3000;
UPDATE orders SET o_totalprice = o_totalprice * 3000;
UPDATE partsupp SET ps_supplycost = ps_supplycost * 3000;
UPDATE part SET p_retailprice = p_retailprice * 3000;
UPDATE customer SET c_acctbal = c_acctbal * 3000;
UPDATE supplier SET s_acctbal = s_acctbal * 3000;
CHECKPOINT;
SQL
echo "wrote $out/tpch_shift.duckdb $out/tpch_big.duckdb"
