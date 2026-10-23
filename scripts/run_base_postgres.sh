#!/usr/bin/env bash
# Read-only base TPC-H accelerators vs PostgreSQL (all 22 queries).
#
#   SF=20 scripts/run_base_postgres.sh [threads-list] [seeds]    (defaults: 16,32 and 20)
#
# 1. builds/installs the accelerator module ($libdir/accelerator_codegen_pg.so) inside the PostgreSQL
#    container and (re)creates public.accel_tpch_qN / public.accel_tpch_qN_parallel,
# 2. correctness: both variants vs the reference SQL on every parametrization, at 1 participant
#    and at each T in the threads list,
# 3. sweep: every timed repetition is a different parametrization (one seed per rep):
#    PostgreSQL and accel_tpch_qN at 1 participant, PostgreSQL and accel_tpch_qN_parallel at T
#    participants.  T participants = max_parallel_workers_per_gather = T-1 on BOTH sides (leader +
#    T-1 parallel workers), so the server needs max_worker_processes and max_parallel_workers >= T-1
#    (this script checks; set them with ALTER SYSTEM + restart).
#
# Env: SF (TPC-H scale factor, default 1; selects eval/bench/tpch/stateless_instances_sf${SF}.jsonl,
#      i.e. Q11's FRACTION = 0.0001/SF), QUERIES (comma list, default all 22),
#      ACCEL_PG_CONTAINER (default accel_pg_test; the module is built and installed in it),
#      TPCH_PG_DB (default accel_tpch_test for SF1, accel_tpch_sf${SF} otherwise — the name
#      backends/postgres/host.sh load creates),
#      PGHOST/PGPORT/PGUSER/PGPASSWORD for the client (default 127.0.0.1:5434 postgres/postgres),
#      OUT_DIR (default eval/bench/tpch/results), SKIP_BUILD=1 to reuse the installed module,
#      BASE_SCHEMA (default public; the build drops every accel_tpch_qN* function in it, so
#      use another schema on a database that also holds the stateful accelerator's functions),
#      PYTHON (default python3; needs psycopg2),
#      PG_FORCE_WORKERS=1 to set parallel_workers = T-1 on the TPC-H tables while PostgreSQL runs
#      at T participants (otherwise its planner may use fewer than T-1 workers); the original
#      reloptions are restored when the sweep ends.  The sweep CSV records PostgreSQL's planned
#      workers either way (pg_workers_planned).
#      PG_BEST_PLAN=1 to time PostgreSQL at each T > 1 under default and zero parallel costs on the
#      warm-up parametrization and use the faster for that query (its cost model keeps Q13 serial).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THREADS="${1:-16,32}"
SEEDS="${2:-20}"
OUT_DIR="${OUT_DIR:-${ROOT}/eval/bench/tpch/results}"
SF="${SF:-1}"
QUERIES="${QUERIES:-$(seq -s, 1 22)}"
if [[ "$SF" == "1" ]]; then DB="${TPCH_PG_DB:-accel_tpch_test}"; else DB="${TPCH_PG_DB:-accel_tpch_sf${SF}}"; fi
INSTANCES="${ROOT}/eval/bench/tpch/stateless_instances_sf${SF}.jsonl"
[[ -f "$INSTANCES" ]] || { echo "no instance file $INSTANCES (build-stateless/stateless_matrix --scale-factor ${SF} --emit-instances ...)" >&2; exit 1; }
CONTAINER="${ACCEL_PG_CONTAINER:-accel_pg_test}"
SCHEMA="${BASE_SCHEMA:-public}"
PYTHON="${PYTHON:-python3}"
FORCE=()
[[ "${PG_FORCE_WORKERS:-0}" == "1" ]] && FORCE=(--force-workers)
[[ "${PG_BEST_PLAN:-0}" == "1" ]] && FORCE+=(--pg-best-plan)
mkdir -p "$OUT_DIR"
STAMP="sf${SF}_$(uname -n | cut -d. -f1)_$(date +%Y%m%d_%H%M%S)"

if ! docker exec "$CONTAINER" psql -U "${PGUSER:-postgres}" -d "$DB" -Atc "SELECT count(*) FROM region" >/dev/null 2>&1; then
  echo "database $DB not reachable in container $CONTAINER (SF${SF}: load it with backends/postgres/host.sh load)" >&2
  exit 1
fi

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
  "$ROOT/scripts/build_base_pg.sh" --schema "$SCHEMA" --db "$DB"
fi

max_t=1
for t in ${THREADS//,/ }; do (( t > max_t )) && max_t=$t; done
read -r mwp mpw < <(docker exec "$CONTAINER" psql -U "${PGUSER:-postgres}" -d "$DB" -At -F ' ' \
  -c "SELECT current_setting('max_worker_processes'), current_setting('max_parallel_workers')")
if (( mwp < max_t - 1 || mpw < max_t - 1 )); then
  echo "server allows max_worker_processes=${mwp}, max_parallel_workers=${mpw}; need >= $((max_t - 1))." >&2
  echo "  docker exec $CONTAINER psql -U postgres -c 'ALTER SYSTEM SET max_worker_processes = 96' -c 'ALTER SYSTEM SET max_parallel_workers = 96'" >&2
  echo "  docker restart $CONTAINER" >&2
  exit 1
fi

echo "=== correctness (db=${DB}, seeds=${SEEDS}, participants 1,${THREADS}) ==="
COMMON=(--db "$DB" --schema "$SCHEMA" --instances "$INSTANCES" --queries "$QUERIES" --seeds "$SEEDS")
"$PYTHON" "$ROOT/scripts/pg_base_bench.py" --mode check "${COMMON[@]}" \
    --threads-list "$THREADS" > "$OUT_DIR/base_postgres_check_${STAMP}.log" || true
tail -n 1 "$OUT_DIR/base_postgres_check_${STAMP}.log"

echo "=== sweep (participants=${THREADS}, seeds=${SEEDS}) ==="
OUT="$OUT_DIR/base_postgres_sweep_${STAMP}.csv"
"$PYTHON" "$ROOT/scripts/pg_base_bench.py" --mode sweep "${COMMON[@]}" \
    --threads-list "$THREADS" "${FORCE[@]}" --out "$OUT"
grep '^#' "$OUT"
echo "raw: $OUT"
