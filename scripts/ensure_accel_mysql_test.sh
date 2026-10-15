#!/usr/bin/env bash
# Idempotent: ensure accel_mysql_test is running; optionally build/install the plugin.
# ACCEL_MYSQL_ENSURE_BUILD=1 (default): build/install the plugin and fail closed if
# live adapter sources are missing (no stale-.so soft pass).
# engine_backend from-scratch sets ACCEL_MYSQL_ENSURE_BUILD=0 (container only).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NAME="${ACCEL_MYSQL_CONTAINER:-accel_mysql_test}"
PORT="${MYSQL_TCP_PORT:-${MYSQL_PORT:-3307}}"
BUILD="${ACCEL_MYSQL_ENSURE_BUILD:-1}"
PASSWORD="${MYSQL_PWD:-${MYSQL_ROOT_PASSWORD:-mysql}}"
SO_PATH="${ACCEL_MYSQL_SO:-/usr/lib/mysql/plugin/accelerator_codegen_mysql.so}"

REQUIRED_SOURCES=(
  "backends/mysql/api/mysql_host.cpp"
  "backends/mysql/engine/mysql_statement_capture.cpp"
)

start_if_needed() {
  if docker ps --format '{{.Names}}' | grep -qx "$NAME"; then
    if docker exec "$NAME" mysqladmin ping -h 127.0.0.1 -uroot -p"${PASSWORD}" --silent >/dev/null 2>&1; then
      echo "[ensure] ${NAME} already running on 127.0.0.1:${PORT}"
      return 0
    fi
    echo "[ensure] ${NAME} exists but not ready; recreating"
  fi
  bash "${ROOT}/scripts/start_accel_mysql_test.sh"
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

if [[ "${BUILD}" == "1" ]] && ! adapter_sources_present; then
  echo "[ensure] adapter sources missing — refusing to treat a stale plugin as success." >&2
  echo "[ensure] Required under ${ROOT}:" >&2
  printf '  - %s\n' "${REQUIRED_SOURCES[@]}" >&2
  echo "[ensure] For from-scratch engine_backend, set ACCEL_MYSQL_ENSURE_BUILD=0 (container only)." >&2
  exit 1
fi

start_if_needed

if [[ "${BUILD}" == "1" ]]; then
  bash "${ROOT}/scripts/build_accel_mysql_plugin.sh"
  if ! docker exec "$NAME" test -f "$SO_PATH"; then
    # Oracle images often install plugins under lib64.
    if ! docker exec "$NAME" bash -lc 'test -f /usr/lib64/mysql/plugin/accelerator_codegen_mysql.so'; then
      echo "[ensure] missing plugin .so after build (${SO_PATH})" >&2
      exit 1
    fi
  fi
else
  echo "[ensure] ACCEL_MYSQL_ENSURE_BUILD=0 — container only (plugin build deferred to gates)"
fi

echo "[ensure] ACCEL_MYSQL_DOCKER ready container=${NAME} MYSQL_HOST=127.0.0.1 MYSQL_TCP_PORT=${PORT} build=${BUILD}"
