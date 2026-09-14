#!/usr/bin/env bash
# Idempotent: ensure accel_pg_test is running; optionally build/install the extension.
# ACCEL_PG_ENSURE_BUILD=1 (default): build/install the MODULE and fail closed if
# live adapter sources are missing (no stale-.so soft pass).
# engine_backend from-scratch sets ACCEL_PG_ENSURE_BUILD=0 (container only).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NAME="${ACCEL_PG_CONTAINER:-accel_pg_test}"
PORT="${PG_PORT:-${PGPORT:-5434}}"
BUILD="${ACCEL_PG_ENSURE_BUILD:-1}"
LOAD_TPCH="${ACCEL_PG_ENSURE_TPCH:-0}"
SO_PATH="/usr/lib/postgresql/17/lib/accelerator_codegen_pg.so"

REQUIRED_SOURCES=(
  "src/generated/postgres/postgres_extension.cpp"
  "src/generated/postgres/runtime.cpp"
  "src/generated/postgres/transition_capture.cpp"
  "src/stateful/postgres/postgres_aux_backend.cpp"
  "src/stateful/postgres/postgres_bridge.cpp"
)

start_if_needed() {
  if docker ps --format '{{.Names}}' | grep -qx "$NAME"; then
    if PGPASSWORD="${PGPASSWORD:-postgres}" psql -h 127.0.0.1 -p "$PORT" -U postgres -d postgres -tAc 'select 1' >/dev/null 2>&1; then
      echo "[ensure] ${NAME} already running on 127.0.0.1:${PORT}"
      return 0
    fi
    echo "[ensure] ${NAME} exists but not ready; recreating"
  fi
  bash "${ROOT}/scripts/start_accel_pg_test.sh"
}

adapter_sources_present() {
  local rel
  for rel in "${REQUIRED_SOURCES[@]}"; do
    if [[ ! -f "${ROOT}/${rel}" ]]; then
      return 1
    fi
  done
  return 0
}

# Fail closed on missing sources before touching Docker (avoids stale-.so soft pass).
if [[ "${BUILD}" == "1" ]] && ! adapter_sources_present; then
  echo "[ensure] adapter sources missing — refusing to treat a stale ${SO_PATH} as success." >&2
  echo "[ensure] Required under ${ROOT}:" >&2
  printf '  - %s\n' "${REQUIRED_SOURCES[@]}" >&2
  echo "[ensure] Live postgres host sources are missing; restore them, then re-run." >&2
  echo "[ensure] For from-scratch engine_backend, set ACCEL_PG_ENSURE_BUILD=0 (container only)." >&2
  exit 1
fi

start_if_needed

if [[ "${BUILD}" == "1" ]]; then
  bash "${ROOT}/scripts/build_accel_postgres_extension.sh"
  if ! docker exec "$NAME" test -f "$SO_PATH"; then
    echo "[ensure] missing ${SO_PATH} after build" >&2
    exit 1
  fi
else
  echo "[ensure] ACCEL_PG_ENSURE_BUILD=0 — container only (extension build deferred to gates)"
fi

if [[ "${LOAD_TPCH}" == "1" ]]; then
  bash "${ROOT}/scripts/setup_accel_postgres_tpch_test_db.sh"
fi

echo "[ensure] ACCEL_PG_DOCKER ready container=${NAME} PGHOST=127.0.0.1 PGPORT=${PORT} build=${BUILD}"
