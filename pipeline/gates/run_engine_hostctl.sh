#!/usr/bin/env bash
# Engine hostctl gate (layer `hostctl`): backends/<engine>/host.sh speaks the host
# contract (pipeline/prompts/engine_backend/host_contract.md). `info` names the engine and
# its verbs; up, load, sql and down work on a fresh instance; a second load is a no-op.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export ROOT

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_engine_gate_context "${ROOT}" 0
export ACCEL_ENGINE_INSTANCE="${ACCEL_ENGINE_INSTANCE}-hostctl"
export ACCEL_ENGINE_STATE_DIR="${ACCEL_ENGINE_STATE_DIR}-hostctl"

JSON="${ACCEL_GATE_OUT:-/tmp/engine_hostctl.json}"
fail() { engine_host down >/dev/null 2>&1 || true; emit_result "${JSON}" engine_hostctl fail "$1" "${2:-}" "engine=${ENGINE_NAME}"; }

[[ -x "${ENGINE_HOST}" ]] || fail "missing or non-executable ${ENGINE_HOST}"
INFO="$(engine_host info)" || fail "host.sh info failed" "${INFO}"
python3 - "${INFO}" "${ENGINE_NAME}" <<'PY' || fail "host.sh info does not match the contract" "${INFO}"
import json, sys
info, engine = json.loads(sys.argv[1]), sys.argv[2]
required = {"info", "up", "down", "load", "build", "sql", "run", "slot"}
assert info.get("engine") == engine, f"engine {info.get('engine')!r} != {engine!r}"
assert isinstance(info.get("embedded"), bool), "embedded must be a bool"
missing = required - set(info.get("verbs") or [])
assert not missing, f"verbs missing: {sorted(missing)}"
PY

[[ -n "${SCHEMA_SQL}" && -n "${ACCEL_DATASET_DIR}" ]] || fail "gate context has no schema_sql / dataset_dir"
engine_steps "up" "load ${SCHEMA_SQL}" "load ${SCHEMA_SQL}" || fail "${ENGINE_FAIL}" "${ENGINE_LOG}"
[[ -s "${ACCEL_ENGINE_STATE_DIR}/env" ]] || fail "up wrote no ${ACCEL_ENGINE_STATE_DIR}/env" "${ENGINE_LOG}"
OUT="$(echo "SELECT 1 AS one;" | engine_host sql -)" || fail "host.sh sql failed" "${OUT}"
[[ "$(tail -1 <<<"${OUT}" | tr -d '[:space:]')" == "1" ]] || fail "host.sh sql: expected CSV with header and the row 1" "${OUT}"
engine_host down >/dev/null 2>&1 || fail "host.sh down failed"
[[ ! -e "${ACCEL_ENGINE_STATE_DIR}/env" ]] || emit_result "${JSON}" engine_hostctl fail "down left ${ACCEL_ENGINE_STATE_DIR}" "" "engine=${ENGINE_NAME}"
emit_result "${JSON}" engine_hostctl pass "" "${ENGINE_LOG}" "engine=${ENGINE_NAME}"
