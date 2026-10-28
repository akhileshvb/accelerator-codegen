#!/usr/bin/env bash
# host.sh build configures again only when the DuckDB the build dir was configured against changes.
set -euo pipefail
HOST="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/host.sh"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
mkdir -p "${work}/bin" "${work}/build"
cat >"${work}/bin/cmake" <<FAKE
#!/bin/sh
if [ "\$1" = --build ]; then echo build >>"${work}/log"; else echo "configure \$ACCEL_DUCKDB_ROOT" >>"${work}/log"
  touch "${work}/build/CMakeCache.txt" "${work}/build/build.ninja"; fi
FAKE
chmod +x "${work}/bin/cmake"
build_with() {
  PATH="${work}/bin:${PATH}" ACCEL_ENGINE_STATE_DIR="${work}/state" ACCEL_BUILD_DIR="${work}/build" \
    ACCEL_DUCKDB_ROOT="$1" bash "${HOST}" build t
}
build_with /fork/a
build_with /fork/a
build_with /fork/b
expected=$'configure /fork/a\nbuild\nbuild\nconfigure /fork/b\nbuild'
[[ "$(<"${work}/log")" == "${expected}" ]] || { echo "unexpected cmake calls:"; cat "${work}/log"; exit 1; }
echo "host.sh build configures again only for another DuckDB"
