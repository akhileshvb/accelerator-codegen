#!/usr/bin/env bash
# Optional aux smoke once a query module + UDF host exist for the engine.
set -euo pipefail
OUT_JSON="${ACCEL_GATE_OUT:-/tmp/engine_aux_smoke.json}"
python3 - <<PY
import json, os
out = os.environ.get("ACCEL_GATE_OUT") or "/tmp/engine_aux_smoke.json"
payload = {
  "gate": "engine_aux_smoke",
  "status": "skip",
  "notes": "aux smoke not configured for this engine yet",
}
open(out, "w", encoding="utf-8").write(json.dumps(payload, indent=2))
raise SystemExit(0)
PY
