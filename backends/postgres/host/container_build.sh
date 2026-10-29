#!/usr/bin/env bash
# Runs inside the instance container: build the extension (+ requested targets) and
# install it into the server's libdir. Mounts: /accel-src (repo), /accel-duckdb (DuckDB
# source, ro), /accel-build (ACCEL_BUILD_DIR). Env: ACCEL_PG_TARGETS, ACCEL_BUILD_JOBS,
# ACCEL_VERUS_LIB_DIR (the Verus kit libraries, relative to the repo), ACCEL_HOST_UID/GID (owner of /accel-build), ACCEL_DUCKDB_SEED (prebuilt DuckDB, optional).
set -euo pipefail

JOBS="${ACCEL_BUILD_JOBS:-$(nproc)}"
PG_LIB=/usr/lib/postgresql/17/lib
TARGETS="${ACCEL_PG_TARGETS//[;,]/ }"

if ! dpkg -s postgresql-server-dev-17 cmake ninja-build patchelf build-essential >/dev/null 2>&1; then
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq postgresql-server-dev-17 build-essential cmake ninja-build patchelf >/dev/null
fi

# DuckDB is built in the container: the host's libduckdb needs a newer glibc.
if [[ ! -f /accel-build/duckdb/src/libduckdb.so ]]; then
  if [[ -n "${ACCEL_DUCKDB_SEED:-}" && -d /accel-duckdb-seed ]]; then
    cp -a /accel-duckdb-seed /accel-build/duckdb
  else
    mkdir -p /accel-build/duckdb
    cd /accel-build/duckdb
    cmake /accel-duckdb -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_EXTENSIONS=tpch
    ninja -j"${JOBS}"
  fi
fi
rm -rf /accel-build/duckdb-sibling
mkdir -p /accel-build/duckdb-sibling/build/release
ln -sfn /accel-duckdb/src /accel-build/duckdb-sibling/src
ln -sfn /accel-build/duckdb/src /accel-build/duckdb-sibling/build/release/src
export ACCEL_DUCKDB_ROOT=/accel-build/duckdb-sibling

PG_CONFIG="$(command -v pg_config)"
mkdir -p /accel-build/accel
cd /accel-build/accel
rm -f CMakeCache.txt
cmake /accel-src -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DACCELERATOR_CODEGEN_BUILD_POSTGRES=ON \
  -DACCEL_CODEGEN_ENGINE=postgres \
  -DACCEL_CALIBRATE_AT_BUILD=OFF \
  ${ACCEL_VERUS_LIB_DIR:+-DACCEL_VERUS_LIB_DIR=/accel-src/${ACCEL_VERUS_LIB_DIR}} \
  -DPG_CONFIG="${PG_CONFIG}" \
  -DACCEL_PG_BINDIR="$(pg_config --bindir)"

ninja accelerator_codegen accelerator_codegen_pg -j"${JOBS}"

for target in ${TARGETS}; do
  ninja -t query "${target}" >/dev/null 2>&1 || { echo "required ninja target missing: ${target}" >&2; exit 1; }
  ninja "${target}" -j"${JOBS}"
done

cp -f accelerator_codegen_pg.so libaccelerator_codegen.so /accel-build/duckdb/src/libduckdb.so "${PG_LIB}/"
# Test-only modules, when a requested target depends on them.
for module in accelerator_codegen_pg_writer_test accelerator_codegen_pg_scan_test; do
  for target in ${TARGETS}; do
    if ninja -t query "${target}" | grep -q "${module}\.so"; then
      ninja "${module}" -j"${JOBS}"
      cp -f "${module}.so" "${PG_LIB}/"
      patchelf --set-rpath "${PG_LIB}" "${PG_LIB}/${module}.so"
      break
    fi
  done
done
patchelf --set-rpath "${PG_LIB}" "${PG_LIB}/accelerator_codegen_pg.so" "${PG_LIB}/libaccelerator_codegen.so"
ldconfig
chown -R "${ACCEL_HOST_UID}:${ACCEL_HOST_GID}" /accel-build
echo "installed accelerator_codegen_pg.so into ${PG_LIB}"
