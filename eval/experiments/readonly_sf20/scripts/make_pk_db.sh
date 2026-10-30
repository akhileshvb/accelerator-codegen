#!/usr/bin/env bash
# Copy the dbgen SF20 database into a new file whose tables declare the standard TPC-H
# primary keys (as GenDB's schema.sql; FKs omitted as GenDB's setup does). DuckDB builds an
# ART index per PRIMARY KEY. Same rows, same column types as dbgen; same DuckDB (capture fork).
# Produces tpch_sf20_pk.duckdb, the database of every other script here. Env: ACCEL_DUCKDB_ROOT, SRC (dbgen SF20 .duckdb), DST.
set -euo pipefail
D=${ACCEL_DUCKDB_ROOT:?ACCEL_DUCKDB_ROOT: root of the DuckDB capture fork}/build/release/duckdb
SRC=${SRC:?SRC: dbgen SF20 .duckdb}; DST=${DST:?DST: output tpch_sf20_pk.duckdb}
rm -f "$DST" "$DST.wal"
declare -A PK=([region]=r_regionkey [nation]=n_nationkey [supplier]=s_suppkey [part]=p_partkey
  [partsupp]="ps_partkey, ps_suppkey" [customer]=c_custkey [orders]=o_orderkey [lineitem]="l_orderkey, l_linenumber")
SQL="ATTACH '$SRC' AS src (READ_ONLY);"
for t in region nation supplier part partsupp customer orders lineitem; do
  cols=$("$D" -readonly "$SRC" -noheader -list -c "SELECT string_agg(column_name || ' ' || data_type || CASE WHEN is_nullable THEN '' ELSE ' NOT NULL' END, ', ' ORDER BY column_index) FROM duckdb_columns() WHERE table_name = '$t' AND schema_name = 'main';")
  SQL+=" CREATE TABLE $t ($cols, PRIMARY KEY (${PK[$t]})); INSERT INTO $t SELECT * FROM src.main.$t;"
done
SQL+=" DETACH src; CHECKPOINT;"
time "$D" "$DST" -c "$SQL"
"$D" -readonly "$DST" -c "SELECT table_name, index_name, sql FROM duckdb_indexes() ORDER BY 1; SELECT table_name, constraint_type, constraint_column_names FROM duckdb_constraints() WHERE constraint_type='PRIMARY KEY' ORDER BY 1; SELECT (SELECT count(*) FROM lineitem) lineitem, (SELECT count(*) FROM orders) orders;"
ls -la "$DST"
