#!/usr/bin/env bash
# Copy the built libraries into the server container, set their rpath, and register the TPC-H accelerators
# (register.sql). Env: BUILD (as build_ext.sh), T (server container, accel_pg_sf20), PG_PORT (5460), DB (accel_tpch_sf20).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD=${BUILD:-$HOME/.cache/accel_pg_sf20/build}; T=${T:-accel_pg_sf20}; L=/usr/lib/postgresql/17/lib
for f in accel/accelerator_codegen_pg.so accel/libaccelerator_codegen.so duckdb/src/libduckdb.so; do docker cp "$BUILD/$f" $T:$L/; done
docker exec -u root $T bash -c "apt-get update -qq && apt-get install -y -qq patchelf >/dev/null &&
  patchelf --set-rpath $L $L/accelerator_codegen_pg.so && patchelf --set-rpath $L $L/libaccelerator_codegen.so && ldconfig"
PGPASSWORD=postgres psql -h 127.0.0.1 -p "${PG_PORT:-5460}" -U postgres -d "${DB:-accel_tpch_sf20}" -v ON_ERROR_STOP=1 \
  -v scripts="$(cd "$HERE/../.." && pwd)" -f "$HERE/register.sql"
