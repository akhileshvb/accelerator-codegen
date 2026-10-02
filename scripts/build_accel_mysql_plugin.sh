#!/usr/bin/env bash
# Build accelerator_codegen (+ optional MySQL plugin / layer harnesses) inside accel_mysql_test.
# From-scratch friendly: missing adapter TUs skip the plugin copy; ninja targets are optional
# unless ACCEL_MYSQL_NINJA_TARGETS is set (layer gates set this).
set -euo pipefail

NAME="${ACCEL_MYSQL_CONTAINER:-accel_mysql_test}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if ! docker ps --format '{{.Names}}' | grep -qx "$NAME"; then
  echo "container ${NAME} not running; start with scripts/start_accel_mysql_test.sh" >&2
  exit 1
fi

EXTRA_TARGETS="${ACCEL_MYSQL_NINJA_TARGETS:-}"
REQUIRE_TARGETS="${ACCEL_MYSQL_REQUIRE_NINJA_TARGETS:-0}"
if [[ -n "${EXTRA_TARGETS}" ]]; then
  REQUIRE_TARGETS=1
fi
EXTRA_TARGETS="${EXTRA_TARGETS//; / }"
EXTRA_TARGETS="${EXTRA_TARGETS//;/ }"
EXTRA_TARGETS="${EXTRA_TARGETS//,/ }"

ENGINE_FLAG="${ACCEL_CODEGEN_ENGINE:-${ACCEL_WORKLOAD_ENGINE_NAME:-mysql}}"

docker exec \
  -e ACCEL_MYSQL_NINJA_TARGETS="${EXTRA_TARGETS}" \
  -e ACCEL_MYSQL_REQUIRE_NINJA_TARGETS="${REQUIRE_TARGETS}" \
  -e ACCEL_CODEGEN_ENGINE="${ENGINE_FLAG}" \
  -e ACCEL_MYSQL_DOCKER=1 \
  "$NAME" bash -lc '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
export ACCEL_MYSQL_DOCKER=1
export ACCEL_CODEGEN_ENGINE="${ACCEL_CODEGEN_ENGINE:-mysql}"

# Ensure ninja is available (downloads prebuilt if package mgr lacks it)
_ensure_ninja() {
  if command -v ninja >/dev/null 2>&1; then return 0; fi
  if command -v ninja-build >/dev/null 2>&1; then return 0; fi

  # Try package manager
  if command -v apt-get >/dev/null 2>&1; then
    apt-get update -qq && apt-get install -y -qq ninja-build >/dev/null 2>&1 && return 0 || true
  elif command -v microdnf >/dev/null 2>&1; then
    microdnf install -y ninja-build >/dev/null 2>&1 && return 0 || true
  elif command -v dnf >/dev/null 2>&1; then
    dnf install -y ninja-build >/dev/null 2>&1 && return 0 || true
  elif command -v yum >/dev/null 2>&1; then
    yum install -y ninja-build >/dev/null 2>&1 && return 0 || true
  fi

  # Fallback: download prebuilt ninja
  echo "installing prebuilt ninja..."
  local NINJA_URL="https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-linux.zip"
  curl -sSL "$NINJA_URL" -o /tmp/ninja.zip
  if command -v unzip >/dev/null 2>&1; then
    unzip -o /tmp/ninja.zip -d /usr/local/bin/ >/dev/null
  elif command -v python3 >/dev/null 2>&1; then
    python3 -c "import zipfile; z=zipfile.ZipFile(\"/tmp/ninja.zip\"); z.extractall(\"/usr/local/bin/\")"
  else
    echo "cannot extract ninja (no unzip or python3)" >&2
    return 1
  fi
  chmod +x /usr/local/bin/ninja
  rm -f /tmp/ninja.zip
}

# Ensure C++ toolchain
if command -v apt-get >/dev/null 2>&1; then
  apt-get update -qq
  apt-get install -y -qq build-essential cmake pkg-config default-libmysqlclient-dev libmysqlclient-dev >/dev/null || \
    apt-get install -y -qq build-essential cmake pkg-config default-libmysqlclient-dev >/dev/null
elif command -v microdnf >/dev/null 2>&1; then
  microdnf install -y gcc-c++ make cmake pkgconf-pkg-config unzip mysql-devel >/dev/null || \
    microdnf install -y gcc-c++ make cmake unzip >/dev/null
elif command -v dnf >/dev/null 2>&1; then
  dnf install -y gcc-c++ make cmake pkgconf-pkg-config mysql-devel >/dev/null || \
    dnf install -y gcc-c++ make cmake >/dev/null
elif command -v yum >/dev/null 2>&1; then
  yum install -y gcc-c++ make cmake pkgconfig mysql-devel >/dev/null || \
    yum install -y gcc-c++ make cmake >/dev/null
else
  echo "no package manager in container to install a C++ toolchain" >&2
  exit 1
fi

_ensure_ninja

# In-container DuckDB build (host Arch libduckdb often needs newer glibc).
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
rm -f CMakeCache.txt
cmake /accel-src -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DACCELERATOR_CODEGEN_BUILD_MYSQL=ON \
  -DACCEL_CODEGEN_ENGINE="${ACCEL_CODEGEN_ENGINE}" \
  -DACCEL_CALIBRATE_AT_BUILD=OFF \
  -DACCEL_ENABLE_PARALLEL_TPCH=OFF \
  -DACCEL_ENABLE_PARALLEL_AUX=OFF

ninja accelerator_codegen -j"$(nproc)"
if ninja -t query accelerator_codegen_mysql >/dev/null 2>&1; then
  ninja accelerator_codegen_mysql -j"$(nproc)"
fi

for t in ${ACCEL_MYSQL_NINJA_TARGETS:-}; do
  if ninja -t query "${t}" >/dev/null 2>&1; then
    ninja "${t}" -j"$(nproc)"
  else
    if [[ "${ACCEL_MYSQL_REQUIRE_NINJA_TARGETS:-0}" == "1" ]]; then
      echo "required ninja target missing: ${t}" >&2
      exit 1
    fi
    echo "skipping absent ninja target: ${t}"
  fi
done

PLUGIN_DIR=""
for d in /usr/lib/mysql/plugin /usr/lib64/mysql/plugin /usr/lib/x86_64-linux-gnu/mariadb19/plugin; do
  if [[ -d "$d" ]]; then
    PLUGIN_DIR="$d"
    break
  fi
done
if [[ -n "${PLUGIN_DIR}" && -f accelerator_codegen_mysql.so ]]; then
  cp -f accelerator_codegen_mysql.so "${PLUGIN_DIR}/"
  cp -f libaccelerator_codegen.so "${PLUGIN_DIR}/" || true
  if [[ -f /accel-build/duckdb/src/libduckdb.so ]]; then
    cp -f /accel-build/duckdb/src/libduckdb.so "${PLUGIN_DIR}/" || true
  fi
  echo "installed accelerator_codegen_mysql.so into ${PLUGIN_DIR}"
else
  echo "plugin .so not built yet (ok for early engine_backend layers)"
fi
'

PASSWORD="${MYSQL_PWD:-${MYSQL_ROOT_PASSWORD:-mysql}}"
# Restart so mysql.plugin / mysql.func autoload the new .so. INSTALL PLUGIN
# and CREATE FUNCTION hit EPERM/SIGSEGV on this Docker volume.
if docker ps -a --format '{{.Names}}' | grep -qx "$NAME"; then
  docker restart "$NAME" >/dev/null
  for _ in $(seq 1 90); do
    if docker exec "$NAME" mysqladmin ping -h 127.0.0.1 -uroot -p"${PASSWORD}" --silent >/dev/null 2>&1; then
      echo "mysqld reloaded with plugin .so"
      # (Re)register the UDFs: base + per-query fragments
      # (src/generated/<bench>/q<N>/mysql/plugin/functions.sql). Idempotent
      # (DROP FUNCTION IF EXISTS + CREATE FUNCTION); INSTALL PLUGIN fails when the
      # plugin is already loaded, so it is skipped. Disable: ACCEL_MYSQL_REGISTER_UDFS=0.
      if [[ "${ACCEL_MYSQL_REGISTER_UDFS:-1}" == "1" ]]; then
        DB="${TPCH_MYSQL_DB:-accel_mysql_test}"
        "$ROOT/scripts/mysql_register_functions.sh" | grep -v '^INSTALL' | \
          docker exec -i "$NAME" mysql -h 127.0.0.1 -P 3306 -uroot -p"${PASSWORD}" "${DB}" \
          || echo "warning: UDF registration reported errors (continuing)" >&2
      fi
      exit 0
    fi
    sleep 1
  done
  echo "timed out waiting for ${NAME} after plugin reload" >&2
  docker logs "$NAME" 2>&1 | tail -20 >&2 || true
  exit 1
fi
