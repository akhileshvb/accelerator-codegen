#!/usr/bin/env bash
# TPC-H update sets (the data of the refresh functions RF1 / RF2) from the official dbgen: dbgen -s SF -U N.
#
#   bash scripts/gen_tpch_refresh_sets.sh [SF=20] [N=40] [OUT=/flash1/akhilvb/tpch/refresh_sf<SF>]
#
# Each set K is 1500 x SF orders (0.1% of the orders) and their lineitems:
#   orders.tbl.uK, lineitem.tbl.uK   RF1 inserts them (keys in the unused gaps of the sparse key space)
#   delete.K                         RF2 deletes these orders (and their lineitems): the specification's RF2 keys
# The sets are meant for the database dbgen's base data came from (the SF20 DuckDB file was made by DuckDB's dbgen,
# which reproduces it): validate_tpch_refresh_sets.py checks that the delete keys exist, the insert keys do not,
# and that the sets do not overlap.
#
# DBGEN_SRC: a TPC-H dbgen 3.0 source tree (default /flash1/akhilvb/tpch/dbgen_src, a copy of the tree at
# /spinning/ziniuw/tpc_data/dbgen without its binaries); it is built in place (gcc, -std=gnu89: the code is K&R C).
# N: the DuckDB harness uses sets across every interval and repetition of a run (about 25 pairs for the default
# campaign), the Postgres / MySQL driver restores the data after each run and reuses sets from 1.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SF=${1:-20}
N=${2:-40}
OUT=${3:-/flash1/akhilvb/tpch/refresh_sf${SF}}
DBGEN_SRC=${DBGEN_SRC:-/flash1/akhilvb/tpch/dbgen_src}
DB=${DB:-${ROOT}/../accelerator-codegen-duckdb/tpch_sf${SF}.duckdb}
PYTHON=${PYTHON:-python3}

[ -f "$DBGEN_SRC/dists.dss" ] || { echo "no dbgen source tree at $DBGEN_SRC (set DBGEN_SRC)" >&2; exit 1; }
if [ ! -x "$DBGEN_SRC/dbgen" ]; then
  (cd "$DBGEN_SRC" && make CC=gcc DATABASE=ORACLE MACHINE=LINUX WORKLOAD=TPCH \
     CFLAGS='-g -DDBNAME=\"dss\" -DLINUX -DORACLE -DTPCH -DRNG_TEST -D_FILE_OFFSET_BITS=64 -std=gnu89 -w' >/dev/null)
fi
mkdir -p "$OUT"
(cd "$DBGEN_SRC" && DSS_PATH="$OUT" ./dbgen -s "$SF" -U "$N" -f -q)
echo "wrote $N update sets of SF$SF to $OUT ($(du -sh "$OUT" | cut -f1)): $(wc -l < "$OUT/delete.1") orders per set"
if [ -f "$DB" ]; then
  "$PYTHON" "$ROOT/scripts/validate_tpch_refresh_sets.py" --db "$DB" --dir "$OUT" --sets "$N"
else
  echo "(no $DB: not validated)"
fi
