#!/usr/bin/env bash
# Build the stateless PostgreSQL accelerator module inside the PostgreSQL
# container, install it into the server's $libdir and (re)create its SQL
# functions accel_tpch_q<N>(...) / accel_tpch_q<N>_parallel(...) in a schema.
#
#   scripts/build_stateless_pg.sh [--lib NAME] [--schema SCHEMA] [--db DB] [--copts "..."]
#                                 [--queries 1,6,14] [--no-register]
#
# Defaults: lib accel_stateless_pg, schema public, db $TPCH_PG_DB or accel_tpch_test,
# container $ACCEL_PG_CONTAINER or accel_pg_test.  The repository (or a git
# worktree nested inside it) must be visible in the container under /accel-src.
# A separate --lib/--schema pair lets several builds coexist on one server
# (each session must only call its own schema's functions).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONTAINER="${ACCEL_PG_CONTAINER:-accel_pg_test}"
LIB=accel_stateless_pg
SCHEMA=public
DB="${TPCH_PG_DB:-accel_tpch_test}"
COPTS="-march=native"
REGISTER=1
QUERIES=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --lib) LIB="$2"; shift 2 ;;
    --schema) SCHEMA="$2"; shift 2 ;;
    --db) DB="$2"; shift 2 ;;
    --copts) COPTS="$2"; shift 2 ;;
    --no-register) REGISTER=0; shift ;;
    --queries) QUERIES="${2//,/;}"; shift 2 ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done

# Map this checkout to its path in the container (/accel-src is the main checkout).
MAIN="$(cd "$(git -C "$ROOT" rev-parse --git-common-dir)/.." && pwd)"
REL="${ROOT#"$MAIN"}"
SRC="/accel-src${REL}"
BUILD="/accel-build/stateless/${LIB}"
[[ "$COPTS" == "-march=native" ]] || BUILD="${BUILD}-copts$(echo -n "$COPTS" | md5sum | cut -c1-6)"

docker exec "$CONTAINER" bash -lc "
set -euo pipefail
if ! command -v ninja >/dev/null || ! command -v pg_config >/dev/null || [ ! -d \"\$(pg_config --includedir-server)\" ]; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq && apt-get install -y -qq postgresql-server-dev-17 build-essential cmake ninja-build >/dev/null
fi
cmake -S '${SRC}/backends/postgres/api/stateless' -B '${BUILD}' -G Ninja -DACCEL_ROOT='${SRC}' \
      -DACCEL_STATELESS_PG_LIB='${LIB}' -DACCEL_STATELESS_COPTS='${COPTS}' -DACCEL_STATELESS_QUERIES='${QUERIES}' >/dev/null
if ! out=\$(ninja -C '${BUILD}' 2>&1); then echo \"\$out\" | grep -E 'error|FAILED|undefined' | head -40; exit 1; fi
LIBDIR=\$(pg_config --pkglibdir)
cp '${BUILD}/${LIB}.so' \"\$LIBDIR/.${LIB}.so.tmp\"
mv -f \"\$LIBDIR/.${LIB}.so.tmp\" \"\$LIBDIR/${LIB}.so\"
"
echo "installed \$libdir/${LIB}.so"

if [[ "$REGISTER" == "1" ]]; then
  docker exec "$CONTAINER" psql -U "${PGUSER:-postgres}" -d "$DB" -v ON_ERROR_STOP=1 -qAt -c "
CREATE SCHEMA IF NOT EXISTS \"${SCHEMA}\";
DO \$\$ DECLARE r record; BEGIN
  -- Drop every overload of accel_tpch_qN / accel_tpch_qN_parallel in the schema (e.g. older
  -- definitions with other argument types) so calls resolve to the stateless kernels.
  FOR r IN SELECT p.oid::regprocedure AS f FROM pg_proc p
           WHERE p.pronamespace = '\"${SCHEMA}\"'::regnamespace
             AND p.proname ~ '^accel_tpch_q[0-9]+(_parallel)?\$' LOOP
    EXECUTE 'DROP FUNCTION ' || r.f || ' CASCADE';
  END LOOP;
END \$\$;
CREATE OR REPLACE FUNCTION \"${SCHEMA}\".accel_stateless_register(text) RETURNS integer
  AS '\$libdir/${LIB}', 'accel_stateless_register' LANGUAGE C;
SELECT 'registered ' || \"${SCHEMA}\".accel_stateless_register('${SCHEMA}') || ' functions in schema ${SCHEMA}';"
fi
