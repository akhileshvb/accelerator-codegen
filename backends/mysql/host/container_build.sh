#!/usr/bin/env bash
# Runs inside the instance container: build the plugin (+ requested targets) and install
# it into the server's plugin directory. Mounts: /accel-src (repo), /accel-duckdb (DuckDB
# source, ro), /accel-build (ACCEL_BUILD_DIR). Env: ACCEL_MYSQL_TARGETS, ACCEL_MYSQL_TEST_UDFS
# (ON|OFF), ACCEL_MYSQL_PLUGIN_VARIANT, ACCEL_BUILD_JOBS, ACCEL_HOST_UID/GID (owner of
# /accel-build), ACCEL_DUCKDB_SEED (prebuilt DuckDB, optional).
set -euo pipefail

JOBS="${ACCEL_BUILD_JOBS:-$(nproc)}"
TARGETS="${ACCEL_MYSQL_TARGETS//[;,]/ }"

if ! command -v g++ >/dev/null || ! command -v cmake >/dev/null; then
  microdnf install -y gcc-c++ make cmake pkgconf-pkg-config unzip >/dev/null
fi
if ! command -v ninja >/dev/null; then
  microdnf install -y ninja-build >/dev/null 2>&1 || {
    curl -sSL https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-linux.zip -o /tmp/ninja.zip
    unzip -o /tmp/ninja.zip -d /usr/local/bin/ >/dev/null
  }
fi

# A MySQL C client library for in-process scans: the server image has none and its -devel
# RPM conflicts with the server package, so unpack MariaDB Connector/C into /opt.
# backends/mysql/backend.cmake looks in /usr and /opt/accel-mysql-client; without it scans use the CLI.
if [[ ! -f /opt/accel-mysql-client/usr/include/mysql/mysql.h ]]; then
  (
    set -e
    microdnf install -y cpio >/dev/null
    rm -rf /tmp/accel-client-rpms && mkdir -p /tmp/accel-client-rpms /opt/accel-mysql-client
    cd /tmp/accel-client-rpms
    microdnf download mariadb-connector-c mariadb-connector-c-devel mariadb-connector-c-config >/dev/null
    rm -f ./*.i686.rpm
    cd /opt/accel-mysql-client
    for f in /tmp/accel-client-rpms/*.rpm; do rpm2cpio "$f" | cpio -idm 2>/dev/null; done
  ) || echo "warning: MySQL client library unavailable; plugin will use CLI scans" >&2
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

mkdir -p /accel-build/accel
cd /accel-build/accel
rm -f CMakeCache.txt
cmake /accel-src -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DACCELERATOR_CODEGEN_BUILD_MYSQL=ON \
  -DACCEL_CODEGEN_ENGINE=mysql \
  -DACCEL_CALIBRATE_AT_BUILD=OFF \
  -DACCEL_ENABLE_PARALLEL_KERNELS=OFF \
  -DACCEL_ENABLE_PARALLEL_AUX=OFF \
  -DACCEL_MYSQL_TEST_UDFS="${ACCEL_MYSQL_TEST_UDFS}"

ninja accelerator_codegen accelerator_codegen_mysql -j"${JOBS}"
for target in ${TARGETS}; do
  ninja -t query "${target}" >/dev/null 2>&1 || { echo "required ninja target missing: ${target}" >&2; exit 1; }
  ninja "${target}" -j"${JOBS}"
done

PLUGIN_DIR="$(mysql_config --plugindir 2>/dev/null || true)"
for dir in "${PLUGIN_DIR}" /usr/lib64/mysql/plugin /usr/lib/mysql/plugin; do
  [[ -d "${dir}" ]] && { PLUGIN_DIR="${dir}"; break; }
done
# Replace by rename, never in place: the running server has the old plugin mapped, and
# overwriting a mapped .so crashes mysqld on shutdown.
install_atomic() {
  cp -f "$1" "${PLUGIN_DIR}/.$(basename "$1").new" && mv -f "${PLUGIN_DIR}/.$(basename "$1").new" "${PLUGIN_DIR}/$(basename "$1")"
}
install_atomic accelerator_codegen_mysql.so
install_atomic libaccelerator_codegen.so
install_atomic /accel-build/duckdb/src/libduckdb.so
chown -R "${ACCEL_HOST_UID}:${ACCEL_HOST_GID}" /accel-build
echo "installed accelerator_codegen_mysql.so (variant=${ACCEL_MYSQL_PLUGIN_VARIANT}) into ${PLUGIN_DIR}"
