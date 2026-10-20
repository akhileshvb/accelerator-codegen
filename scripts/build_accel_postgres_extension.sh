#!/usr/bin/env bash
# Build accelerator_codegen_pg (+ harnesses) inside accel_pg_test and install into PG 17 libdir.
# Test-only modules (accelerator_codegen_pg_writer_test, _scan_test) are built and
# installed next to the production .so when a requested target depends on
# them. Experiments only ever load accelerator_codegen_pg.
# ACCEL_PG_BUILD_JOBS: ninja -j (default: nproc in the container).
set -euo pipefail

NAME="${ACCEL_PG_CONTAINER:-accel_pg_test}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if ! docker ps --format '{{.Names}}' | grep -qx "$NAME"; then
  echo "container ${NAME} not running; start with scripts/start_accel_pg_test.sh" >&2
  exit 1
fi

# Require the backend entry on the host mount (fail closed — no stale-.so soft pass).
if [[ ! -f "${ROOT}/backends/postgres/backend.cmake" ]]; then
  echo "missing backend entry: ${ROOT}/backends/postgres/backend.cmake" >&2
  exit 1
fi

# Extra ninja targets (space-separated). Empty = extension only.
# Gates set ACCEL_PG_NINJA_TARGETS and require those targets to exist and build.
EXTRA_TARGETS="${ACCEL_PG_NINJA_TARGETS:-}"
REQUIRE_TARGETS="${ACCEL_PG_REQUIRE_NINJA_TARGETS:-0}"
if [[ -n "${EXTRA_TARGETS}" ]]; then
  REQUIRE_TARGETS=1
fi

# Normalize separators so "a;b" or "a,b" also work as multiple targets.
EXTRA_TARGETS="${EXTRA_TARGETS//; / }"
EXTRA_TARGETS="${EXTRA_TARGETS//;/ }"
EXTRA_TARGETS="${EXTRA_TARGETS//,/ }"

ENGINE_FLAG=(-DACCEL_CODEGEN_ENGINE="${ACCEL_CODEGEN_ENGINE:-postgres}")

docker exec \
  -e ACCEL_PG_NINJA_TARGETS="${EXTRA_TARGETS}" \
  -e ACCEL_PG_REQUIRE_NINJA_TARGETS="${REQUIRE_TARGETS}" \
  -e ACCEL_CODEGEN_ENGINE="${ACCEL_CODEGEN_ENGINE:-postgres}" \
  -e ACCEL_PG_BUILD_JOBS="${ACCEL_PG_BUILD_JOBS:-}" \
  -e ACCEL_PG_TEST_MODULE="${ACCEL_PG_TEST_MODULE:-0}" \
  "$NAME" bash -lc '
set -euo pipefail
JOBS="${ACCEL_PG_BUILD_JOBS:-$(nproc)}"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq postgresql-server-dev-17 build-essential cmake ninja-build patchelf >/dev/null

# In-container DuckDB build (host Arch libduckdb often needs newer glibc).
if [[ ! -f /accel-build/duckdb/src/libduckdb.so ]]; then
  mkdir -p /accel-build/duckdb
  cd /accel-build/duckdb
  cmake /accel-duckdb -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_EXTENSIONS=tpch
  ninja -j"${JOBS}"
fi
rm -rf /accel-build/duckdb-sibling
mkdir -p /accel-build/duckdb-sibling/build/release
ln -sfn /accel-duckdb/src /accel-build/duckdb-sibling/src
ln -sfn /accel-build/duckdb/src /accel-build/duckdb-sibling/build/release/src
export ACCEL_DUCKDB_ROOT=/accel-build/duckdb-sibling

# postgresql-server-dev-17 puts a complete pg_config on PATH; stamp for FindAccelPostgreSQL.
export PG_CONFIG="$(command -v pg_config)"
export ACCEL_PG_CONFIG="${PG_CONFIG}"
export ACCEL_PG_BINDIR="$(pg_config --bindir)"
export ACCEL_CODEGEN_ENGINE="${ACCEL_CODEGEN_ENGINE:-postgres}"

mkdir -p /accel-build/accel
cd /accel-build/accel
# Always reconfigure after adapter regen (cache may list deleted sources).
rm -f CMakeCache.txt
cmake /accel-src -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DACCELERATOR_CODEGEN_BUILD_POSTGRES=ON \
  -DACCEL_CODEGEN_ENGINE="${ACCEL_CODEGEN_ENGINE}" \
  -DACCEL_CALIBRATE_AT_BUILD=OFF \
  -DPG_CONFIG="${PG_CONFIG}" \
  -DACCEL_PG_BINDIR="${ACCEL_PG_BINDIR}"

ninja accelerator_codegen accelerator_codegen_pg -j"${JOBS}"

# Prefer `ninja -t query` over `ninja -t targets`: the default targets listing
# only includes a small top-level subset (~phony / default), so executables like
# test_postgres_tpch_differential are omitted and gates falsely report "missing".
for t in ${ACCEL_PG_NINJA_TARGETS:-}; do
  if ninja -t query "${t}" >/dev/null 2>&1; then
    ninja "${t}" -j"${JOBS}"
  else
    if [[ "${ACCEL_PG_REQUIRE_NINJA_TARGETS:-0}" == "1" ]]; then
      echo "required ninja target missing: ${t}" >&2
      exit 1
    fi
    echo "skipping absent ninja target: ${t}"
  fi
done

# Test-only MODULEs: when asked for, or when a requested target depends on them.
PG_LIB=/usr/lib/postgresql/17/lib
cp -f accelerator_codegen_pg.so "${PG_LIB}/"
for module in accelerator_codegen_pg_writer_test accelerator_codegen_pg_scan_test; do
  want_test_module="${ACCEL_PG_TEST_MODULE:-0}"
  for t in ${ACCEL_PG_NINJA_TARGETS:-}; do
    if ninja -t query "${t}" 2>/dev/null | grep -q "${module}\.so"; then
      want_test_module=1
    fi
  done
  if [[ "${want_test_module}" == "1" ]]; then
    ninja "${module}" -j"${JOBS}"
    cp -f "${module}.so" "${PG_LIB}/"
    patchelf --set-rpath "${PG_LIB}" "${PG_LIB}/${module}.so" || true
    echo "installed ${module}.so into ${PG_LIB}"
  fi
done
cp -f libaccelerator_codegen.so "${PG_LIB}/"
cp -f /accel-build/duckdb/src/libduckdb.so "${PG_LIB}/"
patchelf --set-rpath "${PG_LIB}" "${PG_LIB}/accelerator_codegen_pg.so" || true
patchelf --set-rpath "${PG_LIB}" "${PG_LIB}/libaccelerator_codegen.so" || true
ldconfig || true
echo "installed accelerator_codegen_pg.so into ${PG_LIB}"
'
