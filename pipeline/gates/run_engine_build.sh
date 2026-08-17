#!/usr/bin/env bash
# Build engine adapter targets (or default release lib when no engine flag).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${ACCEL_PIPELINE_BUILD_DIR:-${ACCEL_BUILD_DIR:-$ROOT/build-release}}"
CTX="${ACCEL_GATE_CONTEXT:-}"
ENGINE=""
BUILD_FLAG=""
OUT_JSON="${ACCEL_GATE_OUT:-}"

if [[ -n "$CTX" && -f "$CTX" ]]; then
  ENGINE=$(python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(d.get('engine_name',''))" "$CTX" 2>/dev/null || true)
  BUILD_FLAG=$(python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print((d.get('cmake') or {}).get('build_flag',''))" "$CTX" 2>/dev/null || true)
fi

mkdir -p "$(dirname "${OUT_JSON:-/tmp/engine_build.json}")"
CFG_ARGS=(-S "$ROOT" -B "$BUILD_DIR")
if [[ -n "$BUILD_FLAG" ]]; then
  CFG_ARGS+=("-D${BUILD_FLAG}=ON")
fi

STATUS="pass"
NOTES="configured build-dir=$BUILD_DIR engine=${ENGINE:-unknown}"
if ! cmake "${CFG_ARGS[@]}" >/tmp/accel_engine_build_cfg.log 2>&1; then
  STATUS="fail"
  NOTES="cmake configure failed; see /tmp/accel_engine_build_cfg.log"
elif ! cmake --build "$BUILD_DIR" -j"$(nproc)" >/tmp/accel_engine_build.log 2>&1; then
  STATUS="fail"
  NOTES="cmake build failed; see /tmp/accel_engine_build.log"
fi

python3 - <<PY
import json, os
out = os.environ.get("ACCEL_GATE_OUT") or "/tmp/engine_build.json"
payload = {"gate": "engine_build", "status": "$STATUS", "notes": """$NOTES""", "engine": "$ENGINE"}
open(out, "w", encoding="utf-8").write(json.dumps(payload, indent=2))
raise SystemExit(0 if "$STATUS" == "pass" else 1)
PY
