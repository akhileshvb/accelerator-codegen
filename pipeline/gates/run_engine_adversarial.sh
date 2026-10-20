#!/usr/bin/env bash
# Build and run the engine's adversarial harness through its host.sh. The validation stage
# also requires >=4 new cases; that count is not checked (the harness has no fixed case marker).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export ROOT

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_engine_gate_context "${ROOT}" 0

# The template's stubs fail on purpose; a source that still has them was never authored.
adversarial_src="$(python3 -c "import json,sys; print((json.loads(sys.argv[1]).get('target_paths') or {}).get('adversarial_test', ''))" "${CTX}")"
if [[ -n "${adversarial_src}" && -f "${ROOT}/${adversarial_src}" ]] && grep -qE "not implemented|TODO" "${ROOT}/${adversarial_src}"; then
  emit_result "${ACCEL_GATE_OUT:-/tmp/engine_adversarial.json}" engine_adversarial fail \
    "${adversarial_src} still has template stubs (not implemented / TODO)" "" "engine=${ENGINE_NAME}"
fi
engine_target_gate engine_adversarial "${ACCEL_GATE_OUT:-/tmp/engine_adversarial.json}" adversarial_test adversarial_test_target
