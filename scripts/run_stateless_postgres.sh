#!/usr/bin/env bash
# Read-only stateless TPC-H accelerators vs PostgreSQL (all 22 queries).
#
#   SF=20 scripts/run_stateless_postgres.sh [threads-list] [seeds]    (defaults: 16,32 and 20)
#
# 1. builds/installs the stateless module ($libdir/accel_stateless_pg.so) inside the PostgreSQL
#    container and (re)creates public.accel_tpch_qN / public.accel_tpch_qN_parallel,
# 2. correctness: both variants vs the reference SQL on every parametrization, at 1 participant
#    and at each T in the threads list,
# 3. sweep: every timed repetition is a different parametrization (one seed per rep):
#    PostgreSQL and accel_tpch_qN at 1 participant, PostgreSQL and accel_tpch_qN_parallel at T
#    participants.  T participants = max_parallel_workers_per_gather = T-1 on BOTH sides (leader +
#    T-1 parallel workers), so the server needs max_worker_processes and max_parallel_workers >= T-1
#    (this script checks; set them with ALTER SYSTEM + restart).
#
# Env: SF (TPC-H scale factor, default 1; selects benchmarks/tpch/stateless_instances_sf${SF}.jsonl,
#      i.e. Q11's FRACTION = 0.0001/SF), QUERIES (comma list, default all 22),
#      ACCEL_PG_CONTAINER (default accel_pg_test; the module is built and installed in it),
#      TPCH_PG_DB (default accel_tpch_test for SF1, accel_tpch_sf${SF} otherwise — the name
#      scripts/setup_sf20_sql_backends.sh uses),
#      PGHOST/PGPORT/PGUSER/PGPASSWORD for the client (default 127.0.0.1:5434 postgres/postgres),
#      OUT_DIR (default benchmarks/tpch/results), SKIP_BUILD=1 to reuse the installed module.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THREADS="${1:-16,32}"
SEEDS="${2:-20}"
OUT_DIR="${OUT_DIR:-${ROOT}/benchmarks/tpch/results}"
SF="${SF:-1}"
QUERIES="${QUERIES:-$(seq -s, 1 22)}"
if [[ "$SF" == "1" ]]; then DB="${TPCH_PG_DB:-accel_tpch_test}"; else DB="${TPCH_PG_DB:-accel_tpch_sf${SF}}"; fi
INSTANCES="${ROOT}/benchmarks/tpch/stateless_instances_sf${SF}.jsonl"
[[ -f "$INSTANCES" ]] || { echo "no instance file $INSTANCES (build-stateless/stateless_matrix --scale-factor ${SF} --emit-instances ...)" >&2; exit 1; }
CONTAINER="${ACCEL_PG_CONTAINER:-accel_pg_test}"
mkdir -p "$OUT_DIR"
STAMP="sf${SF}_$(uname -n | cut -d. -f1)_$(date +%Y%m%d_%H%M%S)"

if ! docker exec "$CONTAINER" psql -U "${PGUSER:-postgres}" -d "$DB" -Atc "SELECT count(*) FROM region" >/dev/null 2>&1; then
  echo "database $DB not reachable in container $CONTAINER (SF${SF}: scripts/setup_sf20_sql_backends.sh postgres)" >&2
  exit 1
fi

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
  "$ROOT/scripts/build_stateless_pg.sh" --schema public --db "$DB"
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
COMMON=(--db "$DB" --instances "$INSTANCES" --queries "$QUERIES" --seeds "$SEEDS")
python3 "$ROOT/scripts/pg_stateless_bench.py" --mode check "${COMMON[@]}" \
    --threads-list "$THREADS" > "$OUT_DIR/stateless_postgres_check_${STAMP}.log" || true
tail -n 1 "$OUT_DIR/stateless_postgres_check_${STAMP}.log"

echo "=== sweep (participants=${THREADS}, seeds=${SEEDS}) ==="
OUT="$OUT_DIR/stateless_postgres_sweep_${STAMP}.csv"
python3 "$ROOT/scripts/pg_stateless_bench.py" --mode sweep "${COMMON[@]}" \
    --threads-list "$THREADS" --out "$OUT"
grep '^#' "$OUT"
echo "raw: $OUT"
