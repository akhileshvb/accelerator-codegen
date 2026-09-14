#!/usr/bin/env bash
# Optional higher-level aux smoke once a real query module + UDF host exist.
# Backend correctness without accelerators is owned by engine_contract.
set -euo pipefail
OUT_JSON="${ACCEL_GATE_OUT:-/tmp/engine_aux_smoke.json}"
python3 - <<PY
import json, os
out = os.environ.get("ACCEL_GATE_OUT") or "/tmp/engine_aux_smoke.json"
payload = {
  "gate": "engine_aux_smoke",
  "status": "skip",
  "notes": (
      "optional; use engine_contract for substrate↔engine correctness without "
      "query accelerators"
  ),
}
open(out, "w", encoding="utf-8").write(json.dumps(payload, indent=2))
raise SystemExit(0)
PY
