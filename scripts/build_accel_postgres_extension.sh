#!/usr/bin/env bash
# Build accelerator_codegen_pg (+ harnesses) inside accel_pg_test and install into PG 17 libdir.
set -euo pipefail

NAME="${ACCEL_PG_CONTAINER:-accel_pg_test}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if ! docker ps --format '{{.Names}}' | grep -qx "$NAME"; then
  echo "container ${NAME} not running; start with scripts/start_accel_pg_test.sh" >&2
  exit 1
fi

# Require adapter sources on the host mount (fail closed — no stale-.so soft pass).
for rel in \
  backends/postgres/engine/extension_engine.cpp \
  backends/postgres/state/postgres_aux_backend.cpp \
  backends/postgres/state/postgres_state_store.cpp
do
  if [[ ! -f "${ROOT}/$rel" ]]; then
    echo "missing adapter source: ${ROOT}/${rel}" >&2
    exit 1
  fi
done

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

ENGINE_FLAG=()
if [[ -n "${ACCEL_CODEGEN_ENGINE:-}" ]]; then
  ENGINE_FLAG+=(-DACCEL_CODEGEN_ENGINE="${ACCEL_CODEGEN_ENGINE}")
elif [[ -n "${ACCEL_WORKLOAD_ENGINE_NAME:-}" ]]; then
  ENGINE_FLAG+=(-DACCEL_CODEGEN_ENGINE="${ACCEL_WORKLOAD_ENGINE_NAME}")
else
  # Postgres TPC-H campaign default: pick up catalog + per-engine kernels.
  ENGINE_FLAG+=(-DACCEL_CODEGEN_ENGINE=postgres)
fi

docker exec \
  -e ACCEL_PG_NINJA_TARGETS="${EXTRA_TARGETS}" \
  -e ACCEL_PG_REQUIRE_NINJA_TARGETS="${REQUIRE_TARGETS}" \
  -e ACCEL_CODEGEN_ENGINE="${ACCEL_CODEGEN_ENGINE:-postgres}" \
  "$NAME" bash -lc '
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq postgresql-server-dev-17 build-essential cmake ninja-build patchelf >/dev/null

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

ninja accelerator_codegen accelerator_codegen_pg -j"$(nproc)"

# Prefer `ninja -t query` over `ninja -t targets`: the default targets listing
# only includes a small top-level subset (~phony / default), so executables like
# test_postgres_tpch_differential are omitted and gates falsely report "missing".
for t in ${ACCEL_PG_NINJA_TARGETS:-}; do
  if ninja -t query "${t}" >/dev/null 2>&1; then
    ninja "${t}" -j"$(nproc)"
  else
    if [[ "${ACCEL_PG_REQUIRE_NINJA_TARGETS:-0}" == "1" ]]; then
      echo "required ninja target missing: ${t}" >&2
      exit 1
    fi
    echo "skipping absent ninja target: ${t}"
  fi
done

# Install MODULE next to libaccelerator_codegen / libduckdb for LOAD.
PG_LIB=/usr/lib/postgresql/17/lib
cp -f accelerator_codegen_pg.so "${PG_LIB}/"
cp -f libaccelerator_codegen.so "${PG_LIB}/"
cp -f /accel-build/duckdb/src/libduckdb.so "${PG_LIB}/"
patchelf --set-rpath "${PG_LIB}" "${PG_LIB}/accelerator_codegen_pg.so" || true
patchelf --set-rpath "${PG_LIB}" "${PG_LIB}/libaccelerator_codegen.so" || true
ldconfig || true
echo "installed accelerator_codegen_pg.so into ${PG_LIB}"
'
