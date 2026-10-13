#!/usr/bin/env bash
# Unattended tail of the SF3000 Postgres setup. Run after (or alongside)
# postgres_tpch_indexes_parallel_sf3000.sh:
#   1. wait for lineitem/orders PK unique indexes (and partsupp PK)
#   2. cancel optional secondary builds (orders custkey/orderdate, partsupp suppkey)
#      so they don't block PK attach / trigger creation (they need ShareRowExclusive)
#   3. wait for the parallel index driver (attaches PKs + ANALYZE)
#   4. install mutation triggers, run Q6 smoke on the write-mix sweep
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB="${TPCH_PG_DB:-accel_tpch_sf3000}"
DRIVER_PID="${1:-}"   # pid of postgres_tpch_indexes_parallel_sf3000.sh
export PGPASSWORD="${PGPASSWORD:-postgres}" PGHOST="${PG_HOST:-127.0.0.1}" PGPORT="${PG_PORT:-5434}" PGUSER="${PGUSER:-postgres}"
q() { psql -d "${DB}" -tAc "$1"; }
log() { echo "[$(date +%T)] $*"; }

valid() { [[ "$(q "select count(*) from pg_index i join pg_class c on c.oid=i.indexrelid where c.relname='$1' and i.indisvalid and i.indisready")" == "1" ]]; }

log "waiting for lineitem_pkey / orders_pkey (cancelling optional secondary builds meanwhile)"
until valid lineitem_pkey && valid orders_pkey; do
  q "select pg_cancel_backend(pid) from pg_stat_activity where query ~ 'CREATE INDEX idx_(orders_(custkey|orderdate)|partsupp_suppkey|lineitem_)'" >/dev/null
  sleep 30
done
log "PK indexes built; attaching primary keys"
psql -d "${DB}" -v ON_ERROR_STOP=1 <<'SQL'
ALTER TABLE lineitem ADD PRIMARY KEY USING INDEX lineitem_pkey;
ALTER TABLE orders   ADD PRIMARY KEY USING INDEX orders_pkey;
SQL
log "ANALYZE"
psql -d "${DB}" -c "ANALYZE"
# invalid leftovers from cancelled CREATE INDEX (none for non-CONCURRENTLY, but be safe)
q "select count(*) from pg_index where not indisvalid" 
log "PKs: $(q "select string_agg(conrelid::regclass::text, ',') from pg_constraint where contype='p' and connamespace='public'::regnamespace")"

log "installing mutation triggers"
bash "${ROOT}/scripts/ensure_accel_postgres_mutation_triggers.sh" "${DB}" || log "trigger install FAILED"

log "smoke: Q6 write-mix"
ACCEL_PG_SKIP_NATIVE=1 ACCEL_PG_REFRESH_SAMPLES=1 TPCH_PG_DB="${DB}" \
  OUT_DIR="${ROOT}/benchmarks/tpch/results_pg_sf3000_smoke" \
  bash "${ROOT}/scripts/run_tpch_pg_matview_vs_aux_write_mix_sweep.sh" smoke
log "FINISH DONE"
