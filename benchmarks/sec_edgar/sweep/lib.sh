# Shared logic for run_readonly.sh and run_write_sweep.sh (sourced, not executed).
ulimit -c 0   # a crashing child must not write a core dump to the root disk
#
# Env knobs (all optional):
#   SWEEP_READS         reads per point (default per script)
#   SWEEP_QPS           virtual reads/s (default 2)
#   SWEEP_QUERIES       space-separated query numbers (default "1 2 3 4 6 24")
#   ACCEL_SWEEP_PARAM_SEED   seed of the random draws (default 1)
#   ACCEL_SWEEP_CHECK_SETS   drawn sets for the end-of-run correctness check (default 4)
#   PG_PARALLEL         max concurrent Postgres instances (tmpfs budget: ~11 GB each; default 4)
#   DUCK_PARALLEL       max concurrent DuckDB copies (default 6)
#   RESULTS_ROOT        where results-<ts> dirs go (default <repo>/benchmarks/sec_edgar/sweep/results)
#   HOSTBUILD           DuckDB host build dir containing benchmark_shared_duckdb_write_interval
#   DUCK_SRC            source DuckDB file (default /home/akhilvb/accelerator-codegen-duckdb/sec_edgar_sf3.duckdb)
#   WORK_ROOT           tmpfs dir for DuckDB copies (default /tmp)
#   DUCK_CHECKPOINT_THRESHOLD  DuckDB checkpoint_threshold for base AND accelerated
#                       mode (default 1TB: no automatic checkpoint inside a timed
#                       commit; "" keeps DuckDB's 16 MiB default)
#   PG_SCRIPTS          dir with start/stop/build_pg_ext scripts (default /home/akhilvb/accel-wt/pg)
#   PG_PORT_BASE        instance for query q uses PG_PORT_BASE+q (default 55460)
#   PG_NAME_PREFIX      instance name prefix (default sw)
#   SKIP_DUCKDB=1 / SKIP_POSTGRES=1
SWEEP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SWEEP_DIR/../../.." && pwd)"
HOSTBUILD="${HOSTBUILD:-/tmp/claude-1056/-home-akhilvb/92fe6fdf-4b00-40be-92d8-2b30af22d1fd/scratchpad/param-host-harness}"
DUCK_SRC="${DUCK_SRC:-/home/akhilvb/accelerator-codegen-duckdb/sec_edgar_sf3.duckdb}"
DUCK_ROOT="${DUCK_ROOT:-/home/akhilvb/accelerator-codegen-duckdb}"
PG_SCRIPTS="${PG_SCRIPTS:-/home/akhilvb/accel-wt/pg}"
PG_PORT_BASE="${PG_PORT_BASE:-55460}"
PG_NAME_PREFIX="${PG_NAME_PREFIX:-sw}"
PG_PARALLEL="${PG_PARALLEL:-4}"
DUCK_PARALLEL="${DUCK_PARALLEL:-6}"
SWEEP_QPS="${SWEEP_QPS:-2}"
SWEEP_QUERIES="${SWEEP_QUERIES:-1 2 3 4 6 24}"
ACCEL_SWEEP_PARAM_SEED="${ACCEL_SWEEP_PARAM_SEED:-1}"
ACCEL_SWEEP_CHECK_SETS="${ACCEL_SWEEP_CHECK_SETS:-4}"
WORK_ROOT="${WORK_ROOT:-/tmp}"
DUCK_CHECKPOINT_THRESHOLD="${DUCK_CHECKPOINT_THRESHOLD-1TB}"
RESULTS_ROOT="${RESULTS_ROOT:-$SWEEP_DIR/results}"

# slowest first for Postgres so the 4-wide pool stays busy
pg_order() { local out="" q; for q in 24 1 6 3 2 4; do [[ " $SWEEP_QUERIES " == *" $q "* ]] && out="$out $q"; done; echo $out; }

sweep_init() {  # sweep_init <label>
  TS="$(date +%Y%m%d-%H%M%S)"
  OUT="$RESULTS_ROOT/$1-$TS"
  WORK="$WORK_ROOT/accel-sweep-$1-$TS"
  mkdir -p "$OUT" "$WORK"
  ln -sfn "$OUT" "$RESULTS_ROOT/$1-latest"
  export OUT WORK
}
log() { echo "[$(date '+%F %T')] $*" | tee -a "$OUT/progress.log"; }

# duck_q <q> "<intervals>" "<param modes>"   (one DB copy, runs every interval x mode)
duck_q() {
  local q=$1 intervals=$2 modes=$3 db="$WORK/duck_q$1.duckdb" first=1 iv mode
  local draws="$WORK/draws_q$q.tsv"
  cp "$DUCK_SRC" "$db"
  python3 -I "$SWEEP_DIR/sweep_params.py" --query "$q" --reads "$SWEEP_READS" \
    --checks "$ACCEL_SWEEP_CHECK_SETS" --seed "$ACCEL_SWEEP_PARAM_SEED" --out "$draws"
  for iv in $intervals; do
    for mode in $modes; do
      log "duckdb q$q interval=$iv params=$mode start"
      local append=()
      [[ $first == 1 ]] || append=(ACCEL_SHARED_SWEEP_APPEND=1)
      if (cd "$REPO" && env ACCEL_SHARED_SWEEP_OUTPUT="$OUT/duckdb_q$q.csv" "${append[@]}" \
            ACCEL_SHARED_SWEEP_READS=$SWEEP_READS ACCEL_SHARED_SWEEP_READER_QPS=$SWEEP_QPS \
            ACCEL_SWEEP_PARAMS=$mode ACCEL_SWEEP_PARAM_FILE="$draws" \
            ACCEL_BENCH_THREADS=1 ACCEL_DUCKDB_ROOT="$DUCK_ROOT" \
            ACCEL_DUCKDB_CHECKPOINT_THRESHOLD="$DUCK_CHECKPOINT_THRESHOLD" \
            "$HOSTBUILD/benchmark_shared_duckdb_write_interval" sec_edgar "$q" "$db" "$iv") \
            >> "$OUT/duckdb_q$q.log" 2>&1; then
        log "duckdb q$q interval=$iv params=$mode done"
      else
        log "duckdb q$q interval=$iv params=$mode FAILED (see duckdb_q$q.log)"
      fi
      first=0
    done
  done
  rm -f "$db" "$db.wal"
}

# pg_q <q> "<comma-separated intervals>" "<param modes>"   (one instance per query)
pg_q() {
  local q=$1 intervals=$2 modes=$3 name="${PG_NAME_PREFIX}_q$1" port=$((PG_PORT_BASE + $1)) mode
  log "postgres q$q: start instance $name:$port"
  if ! "$PG_SCRIPTS/start_pg_instance.sh" "$name" "$port" "$REPO" --reset >> "$OUT/postgres_q$q.log" 2>&1; then
    log "postgres q$q: instance start FAILED"; return 1
  fi
  PGPASSWORD=postgres psql -h 127.0.0.1 -p "$port" -U postgres -d sec_edgar -X -q \
    -v ON_ERROR_STOP=1 -f "$REPO/scripts/postgres_sec_edgar_setup.sql" >> "$OUT/postgres_q$q.log" 2>&1 \
    || { log "postgres q$q: setup FAILED"; "$PG_SCRIPTS/stop_pg_instance.sh" "$name" --purge >/dev/null 2>&1; return 1; }
  "$PG_SCRIPTS/build_pg_ext.sh" "$name" >> "$OUT/postgres_q$q.log" 2>&1 \
    || { log "postgres q$q: extension build FAILED"; "$PG_SCRIPTS/stop_pg_instance.sh" "$name" --purge >/dev/null 2>&1; return 1; }
  for mode in $modes; do
    log "postgres q$q: sweep params=$mode start"
    if ACCEL_REPO="$REPO" SWEEP_READS=$SWEEP_READS SWEEP_QPS=$SWEEP_QPS \
         ACCEL_SWEEP_PARAMS=$mode ACCEL_SWEEP_PARAM_SEED=$ACCEL_SWEEP_PARAM_SEED \
         ACCEL_SWEEP_CHECK_SETS=$ACCEL_SWEEP_CHECK_SETS \
         python3 -I "$SWEEP_DIR/pg_write_sweep.py" --port "$port" --query "$q" \
         --intervals "$intervals" --out "$OUT/postgres_q$q.csv" >> "$OUT/postgres_q$q.log" 2>&1; then
      log "postgres q$q: sweep params=$mode done"
    else
      log "postgres q$q: sweep params=$mode FAILED (see postgres_q$q.log)"
    fi
  done
  "$PG_SCRIPTS/stop_pg_instance.sh" "$name" --purge >> "$OUT/postgres_q$q.log" 2>&1
}

# wps_to_intervals "<writes/s list>" -> space-separated intervals in seconds
wps_to_intervals() { local w; for w in $1; do python3 -c "print(repr(1.0/$w))"; done | tr '\n' ' '; }

# run_all "<duck intervals>" "<pg intervals csv>" "<param modes>"
run_all() {
  export -f duck_q pg_q log
  export SWEEP_DIR REPO HOSTBUILD DUCK_SRC DUCK_ROOT DUCK_CHECKPOINT_THRESHOLD PG_SCRIPTS PG_PORT_BASE PG_NAME_PREFIX \
         SWEEP_READS SWEEP_QPS ACCEL_SWEEP_PARAM_SEED ACCEL_SWEEP_CHECK_SETS
  local d="$1" p="$2" m="$3" pids=()
  if [[ -z "${SKIP_DUCKDB:-}" ]]; then
    ( printf '%s\n' $SWEEP_QUERIES | xargs -P "$DUCK_PARALLEL" -I{} bash -c "duck_q {} '$d' '$m'"; log "ALL DUCKDB DONE" ) &
  fi
  if [[ -z "${SKIP_POSTGRES:-}" ]]; then
    ( printf '%s\n' $(pg_order) | xargs -P "$PG_PARALLEL" -I{} bash -c "pg_q {} '$p' '$m'"; log "ALL POSTGRES DONE" ) &
  fi
  wait
  rm -f "$WORK"/draws_q*.tsv; rmdir "$WORK" 2>/dev/null
  python3 -I "$SWEEP_DIR/summarize.py" "$OUT" > "$OUT/summary.txt" 2>&1
  log "COMPLETE; summary: $OUT/summary.md"
}
