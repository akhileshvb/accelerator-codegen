#!/usr/bin/env bash
# Scan conformance placeholder: passes when scan adapter header exists; optional DuckDB parity later.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CTX="${ACCEL_GATE_CONTEXT:-}"
OUT_JSON="${ACCEL_GATE_OUT:-}"
HEADER=""
ENGINE=""

if [[ -n "$CTX" && -f "$CTX" ]]; then
  HEADER=$(python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print((d.get('target_paths') or {}).get('scan_adapter_header',''))" "$CTX" 2>/dev/null || true)
  ENGINE=$(python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(d.get('engine_name',''))" "$CTX" 2>/dev/null || true)
fi

STATUS="pass"
NOTES="scan adapter present"
if [[ -z "$HEADER" ]]; then
  STATUS="skip"
  NOTES="no scan_adapter_header in gate context"
elif [[ ! -f "$ROOT/$HEADER" ]]; then
  STATUS="fail"
  NOTES="missing scan adapter header: $HEADER"
fi

python3 - <<PY
import json, os
out = os.environ.get("ACCEL_GATE_OUT") or "/tmp/engine_scan.json"
payload = {
  "gate": "engine_scan_conformance",
  "status": "$STATUS",
  "notes": """$NOTES""",
  "engine": "$ENGINE",
  "header": "$HEADER",
}
open(out, "w", encoding="utf-8").write(json.dumps(payload, indent=2))
code = 0 if "$STATUS" in ("pass", "skip") else 1
raise SystemExit(code)
PY
