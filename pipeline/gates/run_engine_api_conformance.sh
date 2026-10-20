#!/usr/bin/env bash
# Gate engine_api_conformance (layer `api`), engine-agnostic: the backend's api/README.md
# manifest matches api/, nothing outside the backend includes a non-api/ header of it,
# then the layer harness (the neutral toy accelerator against api/) is built and run
# like every layer harness (run_engine_layer.sh). The engine comes from the gate context.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CTX="${ACCEL_GATE_CONTEXT:-}"
OUT_JSON="${ACCEL_GATE_OUT:-/tmp/engine_api_conformance.json}"
export ACCEL_GATE_NAME="${ACCEL_GATE_NAME:-engine_api_conformance}"

ENGINE="$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('engine_name') or '')" "${CTX}" 2>/dev/null)"
fail() { # notes log
  python3 - "${OUT_JSON}" "${ENGINE}" "$1" "$2" <<'PY'
import json, sys
out, engine, notes, log = sys.argv[1:5]
json.dump({"gate": "engine_api_conformance", "status": "fail", "engine": engine, "notes": notes,
           "log_tail": log[-4000:]}, open(out, "w"), indent=2)
PY
  exit 1
}

[[ -n "${ENGINE}" ]] || fail "no gate context / engine" ""
log="$(python3 "${ROOT}/pipeline/gates/check_api_manifest.py" "${ROOT}" "${ENGINE}" 2>&1)" \
  || fail "api/README.md does not match api/" "${log}"
log="$(python3 "${ROOT}/scripts/check_layer_includes.py" --root "${ROOT}" 2>&1)" \
  || fail "include boundary: only backends/<engine>/api/ may be included from outside the backend" "${log}"
exec bash "${ROOT}/pipeline/gates/run_engine_layer.sh"
