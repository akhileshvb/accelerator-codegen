#!/usr/bin/env bash
# Build and run the engine's contract harness through its host.sh.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export ROOT

# shellcheck source=aux_gate_common.sh
source "$(dirname "${BASH_SOURCE[0]}")/aux_gate_common.sh"
load_engine_gate_context "${ROOT}" 0

engine_target_gate engine_contract "${ACCEL_GATE_OUT:-/tmp/engine_contract.json}" contract_test contract_test_target
