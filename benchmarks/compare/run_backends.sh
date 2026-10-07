#!/usr/bin/env bash
# C++ accelerator vs the base DBMS on TPC-H SF20, for DuckDB, Postgres and MySQL.
#
#   bash benchmarks/compare/run_backends.sh read      <out_root>   # 1. read-only, single-threaded
#   bash benchmarks/compare/run_backends.sh calibrate <out_root>   # writer-only: how long does one RF commit take?
#   bash benchmarks/compare/run_backends.sh rf        <out_root>   # 2. concurrent RF writer + readers
#   bash benchmarks/compare/run_backends.sh summary   <out_root>
#
# BACKENDS="duckdb postgres mysql" (default all three; they run one after another, never together).
#
# read:       one connection, one query at a time, all 22 queries; per parameter draw the accelerator
#             (first call = its build) and the base DBMS, answers compared.
# calibrate:  the writer alone (no readers), 4 refresh slots back to back (an RF commit + its 6 point writes),
#             accelerators loaded (accel) and not (base).  Prints the slot time T_c and the intervals chosen from it.
# rf:         READERS readers, closed loop over all 22 queries, one writer committing the refresh functions of the
#             TPC-H specification: RF1 of update set 1 (insert its 1500 x SF = 0.1% of the orders, with their
#             lineitems), RF2 of set 1 (delete the orders listed in its delete file), RF1 of set 2, ...  The sets are
#             the official dbgen -U output (scripts/gen_tpch_refresh_sets.sh) in RF_DIR.  Q2, Q11 and Q16 read
#             none of orders / lineitem, so RF never touches them: after every RF commit the writer therefore also
#             commits one point write of each kind on the tables they read (supplier update, part delete + insert,
#             supplier delete + insert, partsupp update), unpaced.  (Nation / region writes are left out: they
#             rebuild many accelerators, and the tables are static in practice.)  The interval
#             between RF commits is a multiple of the calibrated commit time: 1x (writer always busy), 2x, 4x
#             (INTERVAL_MULT).  Base DBMS (no capture) vs accelerator on the same schedule; N_COMMITS (4) RF
#             commits are measured per point.
#
# Every experiment repeats with fresh TPC-H parameter draws (REPEATS, default 3; the same draw for base and
# accel within a repetition; seeds in <dir>/seeds.json); report median [min-max].
#
# Env:  SF (20)  REPEATS (3)  READERS (8)  BACKENDS  INTERVAL_MULT ("1 2 4")  N_COMMITS (4)
#   RF_DIR (/flash1/akhilvb/tpch/refresh_sf<SF>)   the update sets; BATCH_ORDERS (default: whole sets) uses only the
#   first B orders of each set
#   duckdb:    DB (/flash1/akhilvb/tpch/tpch_sf20.duckdb)  WORK  BIN (build-verus-duckdb/aux_compare)  CAP (96G)
#   postgres:  PG_PORT (5434)  PG_DB (accel_tpch_sf20)    PGUSER / PGPASSWORD
#   mysql:     MYSQL_TCP_PORT (3307)  MYSQL_DB (tpch_sf20)  MYSQL_USER / MYSQL_PWD
#   postgres/mysql: QUERY_TIMEOUT (1800 s per baseline query; timeouts are reported, not fatal)
# Needs for postgres/mysql: pip install psycopg2-binary pymysql  (PYTHON=<venv python>).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
MODE=${1:?mode: read | calibrate | rf | summary}
OUT=${2:?out root, e.g. benchmarks/compare/results/backends_$(date +%Y%m%d)}
BACKENDS=${BACKENDS:-duckdb postgres mysql}
SF=${SF:-20}
REPEATS=${REPEATS:-3}
READERS=${READERS:-8}
PYTHON=${PYTHON:-python3}
DB=${DB:-/flash1/akhilvb/tpch/tpch_sf20.duckdb}
BIN=${BIN:-$ROOT/build-verus-duckdb/aux_compare}
CAP=${CAP:-96G}   # a commit that rebuilds state keeps old and new copies alive; 64G may be too tight
PG_DB=${PG_DB:-accel_tpch_sf20}
MYSQL_DB=${MYSQL_DB:-tpch_sf20}
export PG_PORT=${PG_PORT:-5434} MYSQL_TCP_PORT=${MYSQL_TCP_PORT:-3307}
QUERY_TIMEOUT=${QUERY_TIMEOUT:-1800}
RF_DIR=${RF_DIR:-/flash1/akhilvb/tpch/refresh_sf${SF}}
BATCH_ORDERS=${BATCH_ORDERS:-}   # empty: whole update sets (1500 x SF orders)
INTERVAL_MULT=${INTERVAL_MULT:-"1 2 4"}
N_COMMITS=${N_COMMITS:-4}
# readers' queries (RF1/RF2 change orders and lineitem, which all but Q2, Q11, Q16 read; the point writes
# after each RF commit cover those three)
QUERIES=${QUERIES:-"$(seq -s ' ' 1 22)"}
mkdir -p "$OUT"

need_rf_sets() {
  [ -f "$RF_DIR/orders.tbl.u1" ] && [ -f "$RF_DIR/lineitem.tbl.u1" ] && [ -f "$RF_DIR/delete.1" ] || {
    echo "no TPC-H update sets in $RF_DIR: bash scripts/gen_tpch_refresh_sets.sh $SF" >&2; exit 1; }
}

meta() {  # what this ran on
  local d=$1/meta; mkdir -p "$d"
  {
    echo "date: $(date -Is)"; echo "host: $(uname -n)"; echo "cmd: $0 $*"
    echo "sf: $SF repeats: $REPEATS readers: $READERS backends: $BACKENDS rf_dir: $RF_DIR batch_orders: ${BATCH_ORDERS:-whole sets}"
    echo "update sets: $(ls "$RF_DIR"/delete.* 2>/dev/null | wc -l), orders in set 1: $(wc -l < "$RF_DIR/delete.1" 2>/dev/null)"
    echo "interval_mult: $INTERVAL_MULT n_commits: $N_COMMITS queries: $QUERIES"
    echo "git HEAD: $(git rev-parse HEAD)"; echo "uncommitted files: $(git status --short | wc -l)"
    [ -f "$BIN" ] && echo "aux_compare sha256: $(sha256sum "$BIN" | cut -c1-64)"
    echo "nproc: $(nproc)"; lscpu | grep -E 'Model name|Socket|Thread|NUMA node\(s\)'
    echo "load: $(cat /proc/loadavg)"; free -g
    docker ps --format '{{.Names}} {{.Image}} {{.Ports}}' 2>/dev/null
  } > "$d/meta.txt"
  git diff HEAD > "$d/code.diff"
  cp benchmarks/compare/{gen_tpch_params.py,run_compare.py,run_sql_backend.py,rf_intervals.py,aux_compare.cpp} "$d/" 2>/dev/null
  ps -eo pid,user,%cpu,%mem,etime,comm --sort=-%cpu | head -25 > "$d/top_processes.txt"
}

sql_args() {
  case $1 in
    postgres) echo "--backend postgres --db $PG_DB" ;;
    mysql)    echo "--backend mysql --db $MYSQL_DB" ;;
  esac
}

capped() { systemd-run --user --scope -p MemoryMax="$CAP" -p MemorySwapMax=0 "$@"; }

# run_rf <backend> <out dir> <rates> <seconds> <warmup> <readers> <writes flag> [extra args...]
# writes flag: --rf-only (RF commits alone) | --dim-after-rf (each RF commit followed by point writes); update sets from RF_DIR.
run_rf() {
  local b=$1 D=$2 R=$3 S=$4 W=$5 N=$6 WF=$7; shift 7
  if [ "$b" = duckdb ]; then
    # all accelerators in one process; one writer commit maintains all of them; base = DuckDB alone
    capped $PYTHON benchmarks/compare/run_compare.py multi --db "$DB" --sf "$SF" --bin "$BIN" --out "$D" \
      --systems cpp --queries "$QUERIES" --wps "$R" --seconds "$S" --warmup "$W" --readers "$N" \
      --reader-mode rotate $WF --rf-dir "$RF_DIR" ${BATCH_ORDERS:+--batch-orders "$BATCH_ORDERS"} --duckdb-memory "${DUCKDB_MEM:-8GB}" "$@"
  else
    $PYTHON benchmarks/compare/run_sql_backend.py writerate $(sql_args "$b") --sf "$SF" --out "$D" \
      --queries "$QUERIES" --rates "$R" --seconds "$S" --warmup "$W" --readers "$N" $WF \
      --rf-dir "$RF_DIR" ${BATCH_ORDERS:+--batch-orders "$BATCH_ORDERS"} --query-timeout "$QUERY_TIMEOUT" "$@"
  fi
}

case $MODE in
read)
  for b in $BACKENDS; do
    D=$OUT/$b/read; mkdir -p "$D"; meta "$D" "$@"
    echo "=== read $b $(date +%T)"
    if [ "$b" = duckdb ]; then
      # one process per (query, system) builds the accelerator once and answers every repetition's parameter sets
      capped $PYTHON benchmarks/compare/run_compare.py read --db "$DB" --sf "$SF" --bin "$BIN" --out "$D" \
        --systems cpp --repeats "$REPEATS" --sets 8 --reps 5 2>&1 | tee "$D/run.log"
    else
      $PYTHON benchmarks/compare/run_sql_backend.py read $(sql_args "$b") --sf "$SF" --out "$D" \
        --repeats "$REPEATS" --sets 4 --reps 5 --base-reps 2 --query-timeout "$QUERY_TIMEOUT" 2>&1 | tee "$D/run.log"
    fi
  done
  ;;
calibrate)
  need_rf_sets
  for b in $BACKENDS; do
    D=$OUT/$b/calibrate; mkdir -p "$D"; meta "$D" "$@"
    echo "=== calibrate $b: $N_COMMITS refresh slots (an RF commit of a TPC-H update set + 6 point writes), writer alone $(date +%T)"
    # --rates=-1: back to back; the run ends after N_COMMITS slots = 7 x N_COMMITS commits (86400 s is only a cap)
    run_rf "$b" "$D" "-1" 86400 0 0 --dim-after-rf --repeats 1 --sets 1 --max-write-commits "$((7 * N_COMMITS))" 2>&1 | tee "$D/run.log"
    $PYTHON benchmarks/compare/rf_intervals.py "$b" "$D" --mult "$INTERVAL_MULT" --commits "$N_COMMITS" \
      | tee "$D/intervals.json"
  done
  ;;
rf)
  need_rf_sets
  for b in $BACKENDS; do
    C=$OUT/$b/calibrate/intervals.json
    [ -f "$C" ] || { echo "no $C: run '$0 calibrate $OUT' first" >&2; exit 1; }
    TC=$($PYTHON -c "import json;print(json.load(open('$C'))['tc_accel_s'])")
    mapfile -t F < <($PYTHON -c "import json;c=json.load(open('$C'));[print(i,s,w) for i,s,w in zip(c['intervals_s'],c['seconds'],c['warmup'])]")
    for line in "${F[@]}"; do
      read -r IV S W <<<"$line"
      RATE=$($PYTHON -c "print(round(1.0/$IV,6))")
      D=$OUT/$b/rf/interval${IV}s; mkdir -p "$D"; meta "$D" "$@"
      echo "=== rf $b interval=${IV}s ($($PYTHON -c "print(round($IV/$TC,1))")x the accelerator's refresh-slot time of ${TC}s) window=${S}s warmup=${W}s $(date +%T)"
      run_rf "$b" "$D" "$RATE" "$S" "$W" "$READERS" --dim-after-rf --repeats "$REPEATS" --sets 4 2>&1 | tee "$D/run.log"
    done
  done
  ;;
summary)
  for b in $BACKENDS; do
    [ -d "$OUT/$b/read" ] && [ "$b" = duckdb ] && $PYTHON benchmarks/compare/run_compare.py summary "$OUT/$b/read"
    for D in "$OUT/$b"/rf/interval*; do
      [ -d "$D" ] || continue
      echo "=== $b $(basename "$D")"
      if [ "$b" = duckdb ]; then
        $PYTHON benchmarks/compare/run_compare.py summary "$D"
      else
        $PYTHON benchmarks/compare/run_sql_backend.py summary "$D"
      fi
    done
  done
  ;;
*) echo "mode: read | calibrate | rf | summary"; exit 2 ;;
esac
