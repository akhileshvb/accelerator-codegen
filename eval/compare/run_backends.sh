#!/usr/bin/env bash
# C++ accelerator vs the base DBMS on TPC-H SF20, for DuckDB, Postgres and MySQL.
#
#   bash eval/compare/run_backends.sh read      <out_root>   # 1. read-only, single-threaded
#   bash eval/compare/run_backends.sh calibrate <out_root>   # writer-only: how long does one RF commit take?
#   bash eval/compare/run_backends.sh rf        <out_root>   # 2. concurrent RF writer + readers
#   bash eval/compare/run_backends.sh rf-fixed  <out_root>   # 2b. concurrent RF writer + readers at fixed intervals
#   bash eval/compare/run_backends.sh cycle     <out_root>   # 3. per-query workload speedup per write interval (DuckDB)
#   bash eval/compare/run_backends.sh summary   <out_root>
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
# rf-fixed:   as rf, but at fixed wall-clock intervals between RF commits instead of calibrated multiples:
#             FIXED_INTERVALS (default "60 30 15 10 5" s, the DuckDB schedule).  Every interval gets the same
#             window FIXED_WINDOW (180 s) and warmup FIXED_WARMUP (30 s), so the points differ only in the write
#             rate; a window is extended (to MAX_WINDOW, 900 s) until every query has MIN_READS (3) reads in it.
#             Repetitions are the outer loop and the interval order reverses every other repetition, so drift
#             of the one shared worker over the sweep does not line up with the rate.  An interval below the
#             commit time saturates the writer (back to back, lag grows): the run reports the achieved rows/s.
#             Postgres: PG_WORKERS (default floor(nproc / READERS) - 1) parallel workers per session for base and
#             accel alike, so READERS concurrent readers together get the whole machine.  With PG_SHARED=1 all
#             accelerator points run first on one shared-state worker (state built once), then all base points;
#             SEED (default random) fixes the parameter draws so base and accel stay paired.
#
# cycle:      (DuckDB only) the overnight experiment: per query, ONE accelerator alone in its process; the TPC-H refresh
#             functions applied one commit at a time from the real update sets (PAIRS pairs of RF1 / RF2, default 4), a
#             burst of BURST reads (default 200) after every commit, on a base run (plain SQL, DuckDB reads) and an accelerator
#             run (maintenance by the policy with the query's SF20 cost model), per repetition.  plot_cycle.py then computes
#             each query's workload speedup at CYCLE_INTERVALS (seconds, default "300 120 60") and writes
#             <out>/duckdb/cycle/cycle_speedup.{png,csv}.  DUCKDB_THREADS=N: DuckDB (and Q13) use N threads.
#
# Every experiment repeats with fresh TPC-H parameter draws (REPEATS, default 3; the same draw for base and
# accel within a repetition; seeds in <dir>/seeds.json); report median [min-max].
#
# Env:  SF (20)  REPEATS (3)  READERS (8)  BACKENDS  INTERVAL_MULT ("1 2 4")  N_COMMITS (4)
#   RF_DIR (/flash1/akhilvb/tpch/refresh_sf<SF>)   the update sets; BATCH_ORDERS (default: whole sets) uses only the
#   first B orders of each set
#   duckdb:    DB (../accelerator-codegen-duckdb/tpch_sf20.duckdb)  WORK (/flash1/akhilvb/tpch/work: the per-run copies)  BIN (build-verus-duckdb/aux_compare)  CAP (96G)
#   postgres:  PG_PORT (5434)  PG_DB (accel_tpch_sf20)    PGUSER / PGPASSWORD  PG_WORKERS (rf-fixed)
#              ONLY_PHASES (rf-fixed): the systems to run, of base / accel / mv; mv reads the materialized views
#              of mv_defs.py in schema MV_SCHEMA (default public)
#              PG_SHARED (1): one copy of the accelerators in the extension's shared-state worker, maintained
#              by the deltas each writer commit ships to it (accel.shared_state; the analogue of DuckDB's one
#              process).  PG_SHARED=0: a copy per connection, which every other connection's write invalidates,
#              so each reader rebuilds its copy in full on its next query.
#   mysql:     MYSQL_TCP_PORT (3307)  MYSQL_DB (tpch_sf20)  MYSQL_USER / MYSQL_PWD
#   postgres/mysql: QUERY_TIMEOUT (1800 s per baseline query; timeouts are reported, not fatal)
# Needs for postgres/mysql: pip install psycopg2-binary pymysql  (PYTHON=<venv python>).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
MODE=${1:?mode: read | calibrate | rf | cycle | summary}
OUT=${2:?out root, e.g. eval/compare/results/backends_$(date +%Y%m%d)}
BACKENDS=${BACKENDS:-duckdb postgres mysql}
SF=${SF:-20}
REPEATS=${REPEATS:-3}
READERS=${READERS:-8}
PYTHON=${PYTHON:-python3}
DB=${DB:-${ROOT}/../accelerator-codegen-duckdb/tpch_sf20.duckdb}
BIN=${BIN:-$ROOT/build-verus-duckdb/aux_compare}
CAP=${CAP:-96G}   # a commit that rebuilds state keeps old and new copies alive; 64G may be too tight
PG_DB=${PG_DB:-accel_tpch_sf20}
MYSQL_DB=${MYSQL_DB:-tpch_sf20}
PG_SHARED=${PG_SHARED:-1}
export PG_PORT=${PG_PORT:-5434} MYSQL_TCP_PORT=${MYSQL_TCP_PORT:-3307}
QUERY_TIMEOUT=${QUERY_TIMEOUT:-1800}
RF_DIR=${RF_DIR:-/flash1/akhilvb/tpch/refresh_sf${SF}}
BATCH_ORDERS=${BATCH_ORDERS:-}   # empty: whole update sets (1500 x SF orders)
INTERVAL_MULT=${INTERVAL_MULT:-"1 2 4"}
N_COMMITS=${N_COMMITS:-4}
PAIRS_CYC=${PAIRS:-4}   # RF1+RF2 pairs per cycle run: enough commits to include periodic compactions (Q20: every 3-4)
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
    echo "sf: $SF repeats: $REPEATS readers: $READERS backends: $BACKENDS rf_dir: $RF_DIR batch_orders: ${BATCH_ORDERS:-whole sets} pg_shared: $PG_SHARED"
    echo "update sets: $(ls "$RF_DIR"/delete.* 2>/dev/null | wc -l), orders in set 1: $(wc -l < "$RF_DIR/delete.1" 2>/dev/null)"
    echo "interval_mult: $INTERVAL_MULT n_commits: $N_COMMITS queries: $QUERIES"
    echo "git HEAD: $(git rev-parse HEAD)"; echo "uncommitted files: $(git status --short | wc -l)"
    [ -f "$BIN" ] && echo "aux_compare sha256: $(sha256sum "$BIN" | cut -c1-64)"
    echo "nproc: $(nproc)"; lscpu | grep -E 'Model name|Socket|Thread|NUMA node\(s\)'
    echo "load: $(cat /proc/loadavg)"; free -g
    docker ps --format '{{.Names}} {{.Image}} {{.Ports}}' 2>/dev/null
  } > "$d/meta.txt"
  git diff HEAD > "$d/code.diff"
  cp eval/compare/{gen_tpch_params.py,run_compare.py,run_sql_backend.py,rf_intervals.py,aux_compare.cpp} "$d/" 2>/dev/null
  ps -eo pid,user,%cpu,%mem,etime,comm --sort=-%cpu | head -25 > "$d/top_processes.txt"
}

sql_args() {
  case $1 in
    postgres) echo "--backend postgres --db $PG_DB $([ "$PG_SHARED" = 1 ] && echo --shared-state)" ;;
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
    capped $PYTHON eval/compare/run_compare.py multi --db "$DB" --sf "$SF" --bin "$BIN" --out "$D" \
      --systems cpp --queries "$QUERIES" --wps "$R" --seconds "$S" --warmup "$W" --readers "$N" \
      --reader-mode rotate $WF --rf-dir "$RF_DIR" ${BATCH_ORDERS:+--batch-orders "$BATCH_ORDERS"} --duckdb-memory "${DUCKDB_MEM:-8GB}" "$@"
  else
    $PYTHON eval/compare/run_sql_backend.py writerate $(sql_args "$b") --sf "$SF" --out "$D" \
      --queries "$QUERIES" --rates "$R" --seconds "$S" --warmup "$W" --readers "$N" $WF \
      --rf-dir "$RF_DIR" ${BATCH_ORDERS:+--batch-orders "$BATCH_ORDERS"} --query-timeout "$QUERY_TIMEOUT" "$@"
  fi
}

case $MODE in
rf-fixed)
  need_rf_sets
  PG_WORKERS=${PG_WORKERS:-$(( $(nproc) / READERS - 1 ))}
  SEED=${SEED:-$(od -An -N4 -tu4 /dev/urandom | tr -d ' ')}   # one draw base for every point: base and accel paired
  IVS=${FIXED_INTERVALS:-60 30 15 10 5}
  if [[ " $BACKENDS " == *" postgres "* ]]; then
    # the tables' parallel_workers = PG_WORKERS once for the whole experiment (no DDL between the runs that
    # share one worker's state); the originals are saved first and restored on any exit
    # (ONLY_PHASES=mv: the tables of MV_SCHEMA, which may be a copy next to the public tables other runs use)
    RELOPT_SCHEMA=public; [ "${ONLY_PHASES:-}" = mv ] && RELOPT_SCHEMA=${MV_SCHEMA:-public}
    RELOPT_FILE=$OUT/postgres/reloptions_original.json; mkdir -p "$OUT/postgres"
    $PYTHON eval/compare/run_sql_backend.py reloptions --db "$PG_DB" --schema "$RELOPT_SCHEMA" --file "$RELOPT_FILE" --set "$PG_WORKERS" || exit 1
    trap '$PYTHON eval/compare/run_sql_backend.py reloptions --db "$PG_DB" --schema "$RELOPT_SCHEMA" --file "$RELOPT_FILE" --restore' EXIT
  fi
  for b in $BACKENDS; do
    # Postgres shared state: every accelerator point first, on ONE worker that builds its state once (the
    # first run's priming; ~1 h at SF20, serial) and is maintained by the deltas of every later commit
    # (writer and restore alike, all with the capture triggers on); then every base point (triggers off, so
    # the worker is stopped before each).  Priming is outside the measured window either way.
    PHASES="base accel"; [ "$b" = postgres ] && [ "$PG_SHARED" = 1 ] && PHASES="accel base"
    # e.g. "accel": one system only (the other's runs exist elsewhere); "mv" (postgres): the materialized-view
    # baseline (mv_defs.py create --schema MV_SCHEMA first), on the tables of schema MV_SCHEMA (default public)
    [ -n "${ONLY_PHASES:-}" ] && PHASES=$ONLY_PHASES
    FIRST=1
    S=${FIXED_WINDOW:-180}; W=${FIXED_WARMUP:-30}
    for SYS in $PHASES; do
      for ((K = ${FIRST_REP:-0}; K < REPEATS; K++)); do   # FIRST_REP: resume a sweep at that repetition
      ORDER=$IVS; [ $((K % 2)) = 1 ] && ORDER=$(echo $IVS | tr ' ' '\n' | tac | tr '\n' ' ')
      for IV in $ORDER; do
        RATE=$($PYTHON -c "print(round(1.0 / $IV, 6))")
        D=$OUT/$b/rf-fixed/interval${IV}s; mkdir -p "$D"; meta "$D" "$@"
        echo "pg_workers: $PG_WORKERS (per session, base and accel) seed: $SEED phases: $PHASES" >> "$D/meta/meta.txt"
        EXTRA=(--seed "$SEED")
        # the SQL harness runs one repetition per call (DuckDB's runs every repetition in one call)
        [ "$b" != duckdb ] && EXTRA+=(--only-rep "$K" --min-reads "${MIN_READS:-3}" --max-seconds "${MAX_WINDOW:-900}")
        if [ "$b" = postgres ]; then
          EXTRA+=(--pg-workers "$PG_WORKERS")
          if [ "$PHASES" = "accel base" ] || [ -n "${ONLY_PHASES:-}" ]; then
            EXTRA+=(--systems "$SYS")
            # (mv: no shared worker, so no --worker-restart)
            # mv: the views' schema, and smaller write statements (pg_ivm maintenance per statement is superlinear)
            [ "$SYS" = mv ] && EXTRA+=(--schema "${MV_SCHEMA:-public}" --chunk "${MV_CHUNK:-500}")
            if [ "$SYS" = accel ]; then
              # FIRST_RESTART=never: keep a worker that is already running and valid (no writes with the
              # triggers off since it started), e.g. one whose build an interrupted run left behind
              EXTRA+=(--worker-restart "$([ $FIRST = 1 ] && echo "${FIRST_RESTART:-first}" || echo never)"); FIRST=0
            fi
          fi
        fi
        echo "=== rf-fixed $b ${SYS} rep $K interval=${IV}s window=${S}s (min reads ${MIN_READS:-3}, max ${MAX_WINDOW:-900}s) warmup=${W}s readers=$READERS seed=$SEED$([ "$b" = postgres ] && echo " pg_workers=$PG_WORKERS") $(date +%T)"
        run_rf "$b" "$D" "$RATE" "$S" "$W" "$READERS" --dim-after-rf --repeats "$REPEATS" --sets 4 "${EXTRA[@]}" 2>&1 | tee -ia "$D/run.log"
      done
      [ "$b" = duckdb ] && break
      done
      # (otherwise both systems ran per invocation; ONLY_PHASES runs one system per invocation)
      [ "$b" = postgres ] && { [ "$PHASES" = "accel base" ] || [ -n "${ONLY_PHASES:-}" ]; } || break
    done
  done
  ;;
read)
  for b in $BACKENDS; do
    D=$OUT/$b/read; mkdir -p "$D"; meta "$D" "$@"
    echo "=== read $b $(date +%T)"
    if [ "$b" = duckdb ]; then
      # one process per (query, system) builds the accelerator once and answers every repetition's parameter sets
      capped $PYTHON eval/compare/run_compare.py read --db "$DB" --sf "$SF" --bin "$BIN" --out "$D" \
        --systems cpp --repeats "$REPEATS" --sets 8 --reps 5 2>&1 | tee "$D/run.log"
    else
      $PYTHON eval/compare/run_sql_backend.py read $(sql_args "$b") --sf "$SF" --out "$D" \
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
    $PYTHON eval/compare/rf_intervals.py "$b" "$D" --mult "$INTERVAL_MULT" --commits "$N_COMMITS" \
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
cycle)
  need_rf_sets
  D=$OUT/duckdb/cycle; mkdir -p "$D"; meta "$D" "$@"
  echo "=== cycle duckdb: $(echo $QUERIES | wc -w) queries x $REPEATS repetition(s), $PAIRS_CYC RF pairs, burst ${BURST:-200} $(date +%T)"
  capped $PYTHON eval/compare/run_compare.py cycle --db "$DB" --sf "$SF" --bin "$BIN" --out "$D" --queries "$QUERIES" \
    --repeats "$REPEATS" --sets 8 --pairs "$PAIRS_CYC" --burst "${BURST:-200}" --rf-dir "$RF_DIR" \
    ${BATCH_ORDERS:+--batch-orders "$BATCH_ORDERS"} ${DUCKDB_THREADS:+--duckdb-threads "$DUCKDB_THREADS"} \
    --duckdb-memory "${DUCKDB_MEM:-8GB}" 2>&1 | tee "$D/run.log"
  $PYTHON eval/compare/plot_cycle.py "$D" --intervals ${CYCLE_INTERVALS:-300 120 60} 2>&1 | tee "$D/plot.log"
  ;;
summary)
  for b in $BACKENDS; do
    [ -d "$OUT/$b/read" ] && [ "$b" = duckdb ] && $PYTHON eval/compare/run_compare.py summary "$OUT/$b/read"
    for D in "$OUT/$b"/rf/interval* "$OUT/$b"/rf-fixed/interval*; do
      [ -d "$D" ] || continue
      echo "=== $b $(basename "$D")"
      if [ "$b" = duckdb ]; then
        $PYTHON eval/compare/run_compare.py summary "$D"
      else
        $PYTHON eval/compare/run_sql_backend.py summary "$D"
      fi
    done
  done
  ;;
*) echo "mode: read | calibrate | rf | cycle | summary"; exit 2 ;;
esac
