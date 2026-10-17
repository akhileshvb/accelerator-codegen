#!/usr/bin/env bash
# Build accelerator_codegen_pg from this tree (mounted read-only) in a builder container of the same image as the
# server, with the build dir on $BUILD. Env: BUILD (default $HOME/.cache/accel_pg_sf20/build), DUCKDB (DuckDB fork
# source, default $ROOT/../duckdb-dbcapture), DUCKDB_BUILD (optional prebuilt DuckDB build dir to seed $BUILD/duckdb
# with; otherwise it is built in the container, ~30+ min), BUILD_CPUS (0-31), JOBS (12).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
B=accel_pg_sf20_build
BUILD=${BUILD:-$HOME/.cache/accel_pg_sf20/build}
DUCKDB=${DUCKDB:-$(dirname "$ROOT")/duckdb-dbcapture}
mkdir -p "$BUILD"
docker rm -f $B >/dev/null 2>&1 || true
docker run -d --name $B --cpuset-cpus "${BUILD_CPUS:-0-31}" -v "$ROOT":/accel-src:ro -v "$DUCKDB":/accel-duckdb:ro \
  -v "$BUILD":/accel-build postgres:17 sleep infinity >/dev/null
if [ -n "${DUCKDB_BUILD:-}" ]; then mkdir -p "$BUILD/duckdb" && cp -au "$DUCKDB_BUILD"/. "$BUILD/duckdb/"; fi
docker exec -e JOBS="${JOBS:-12}" $B bash -lc '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq postgresql-server-dev-17 build-essential cmake ninja-build patchelf >/dev/null
if [ ! -f /accel-build/duckdb/src/libduckdb.so ]; then
  mkdir -p /accel-build/duckdb && cd /accel-build/duckdb
  cmake /accel-duckdb -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_EXTENSIONS=tpch && nice ninja -j"$JOBS"
fi
rm -rf /accel-build/duckdb-sibling; mkdir -p /accel-build/duckdb-sibling/build/release
ln -sfn /accel-duckdb/src /accel-build/duckdb-sibling/src
ln -sfn /accel-build/duckdb/src /accel-build/duckdb-sibling/build/release/src
export ACCEL_DUCKDB_ROOT=/accel-build/duckdb-sibling
export PG_CONFIG="$(command -v pg_config)"
mkdir -p /accel-build/accel && cd /accel-build/accel && rm -f CMakeCache.txt
cmake /accel-src -G Ninja -DCMAKE_BUILD_TYPE=Release -DACCELERATOR_CODEGEN_BUILD_POSTGRES=ON -DACCEL_CODEGEN_ENGINE=postgres \
  -DACCEL_CALIBRATE_AT_BUILD=OFF -DPG_CONFIG="$PG_CONFIG" -DACCEL_PG_BINDIR="$(pg_config --bindir)"
nice ninja accelerator_codegen accelerator_codegen_pg -j"$JOBS"
'
