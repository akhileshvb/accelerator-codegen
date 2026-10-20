#!/usr/bin/env bash
# Engine build gate: the backend's host.sh answers `info` for this engine, brings the
# instance up and builds the backend; the backend must name no query.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export ROOT

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_engine_gate_context "${ROOT}" 0

JSON="${ACCEL_GATE_OUT:-/tmp/engine_build.json}"
if [[ ! -x "${ENGINE_HOST}" ]]; then
  emit_result "${JSON}" engine_build fail "missing or non-executable ${ENGINE_HOST}" "" "engine=${ENGINE_NAME}"
fi
if [[ "$(engine_host info | python3 -c "import json,sys; print(json.load(sys.stdin).get('engine',''))")" != "${ENGINE_NAME}" ]]; then
  emit_result "${JSON}" engine_build fail "host.sh info does not report engine ${ENGINE_NAME}" "" "engine=${ENGINE_NAME}"
fi
if ! engine_steps "up" "build"; then
  emit_result "${JSON}" engine_build fail "${ENGINE_FAIL}" "${ENGINE_LOG}" "engine=${ENGINE_NAME}"
fi
# The build really includes the backend: some CMake cache under the build dir (the host's,
# or the one the engine's own build environment writes) has this backend switched on and was
# configured with $ACCEL_CMAKE_ARGS' build type.
switch="ACCELERATOR_CODEGEN_BUILD_${ENGINE_NAME^^}"
build_type="$(sed -nE 's/.*-DCMAKE_BUILD_TYPE=([^ ]+).*/\1/p' <<<"${ACCEL_CMAKE_ARGS}")"
configured() {
  local cache
  while IFS= read -r cache; do
    grep -qx "${switch}:BOOL=ON" "${cache}" && grep -qx "CMAKE_BUILD_TYPE:STRING=${build_type}" "${cache}" && return 0
  done < <(find "${ACCEL_BUILD_DIR}" -maxdepth 3 -name CMakeCache.txt 2>/dev/null)
  return 1
}
if ! configured; then
  emit_result "${JSON}" engine_build fail \
    "the build dir is not configured with \$ACCEL_CMAKE_ARGS (${switch}=ON, CMAKE_BUILD_TYPE=${build_type}): host.sh build must configure it" \
    "${ENGINE_LOG}" "engine=${ENGINE_NAME}"
fi
if ! CHECK_LOG="$(python3 "${ROOT}/scripts/check_backend_query_free.py" --engine "${ENGINE_NAME}" 2>&1)"; then
  emit_result "${JSON}" engine_build fail "per-query code in the backend (registries + generic dispatch only)" \
    "${CHECK_LOG}" "engine=${ENGINE_NAME}"
fi
emit_result "${JSON}" engine_build pass "host up + build ok" "${ENGINE_LOG}" "engine=${ENGINE_NAME}"
