#!/usr/bin/env bash
# Build accelerator_codegen_postgres inside accel_pg_test and install into PG 17 libdir.
set -euo pipefail

NAME="${ACCEL_PG_CONTAINER:-accel_pg_test}"
if ! docker ps --format '{{.Names}}' | grep -qx "$NAME"; then
  echo "container ${NAME} not running; start with scripts/start_accel_pg_test.sh" >&2
  exit 1
fi

docker exec "$NAME" bash -lc '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq postgresql-server-dev-17 build-essential cmake ninja-build patchelf >/dev/null

# In-container DuckDB build (host Arch libduckdb needs newer glibc).
if [[ ! -f /accel-build/duckdb/src/libduckdb.so ]]; then
  mkdir -p /accel-build/duckdb
  cd /accel-build/duckdb
  cmake /accel-duckdb -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_EXTENSIONS=tpch
  ninja -j"$(nproc)"
fi
rm -rf /accel-build/duckdb-sibling
mkdir -p /accel-build/duckdb-sibling/build/release
ln -sfn /accel-duckdb/src /accel-build/duckdb-sibling/src
ln -sfn /accel-build/duckdb/src /accel-build/duckdb-sibling/build/release/src
export ACCEL_DUCKDB_ROOT=/accel-build/duckdb-sibling

mkdir -p /accel-build/accel
cd /accel-build/accel
if [[ ! -f CMakeCache.txt ]]; then
  cmake /accel-src -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DACCELERATOR_CODEGEN_BUILD_POSTGRES=ON -DACCEL_CALIBRATE_AT_BUILD=OFF
fi
ninja accelerator_codegen accelerator_codegen_postgres -j"$(nproc)"
cp -f accelerator_codegen_postgres.so /usr/lib/postgresql/17/lib/
cp -f libaccelerator_codegen.so /usr/lib/postgresql/17/lib/
cp -f /accel-build/duckdb/src/libduckdb.so /usr/lib/postgresql/17/lib/
cp -f /accel-src/src/runtime/postgres/accelerator_codegen_postgres.control \
  /usr/share/postgresql/17/extension/
cp -f /accel-src/src/runtime/postgres/accelerator_codegen_postgres--0.1.sql \
  /usr/share/postgresql/17/extension/
patchelf --set-rpath /usr/lib/postgresql/17/lib \
  /usr/lib/postgresql/17/lib/accelerator_codegen_postgres.so
patchelf --set-rpath /usr/lib/postgresql/17/lib \
  /usr/lib/postgresql/17/lib/libaccelerator_codegen.so
ldconfig || true
echo "installed accelerator_codegen_postgres into /usr/lib/postgresql/17/lib"
'
