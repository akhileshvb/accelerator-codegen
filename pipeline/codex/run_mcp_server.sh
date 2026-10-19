#!/usr/bin/env bash
# Launch accel_pipeline MCP over stdio for Codex exec.
# Codex may sanitize child env; prefer sidecar written by CodexExecBackend.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"
ORCH="$REPO_ROOT/pipeline/orchestrator"

# Prefer the per-worker / pipeline CODEX_HOME sidecar.
for home in "${ACCEL_PIPELINE_CODEX_HOME:-}" "${CODEX_HOME:-}"; do
  if [[ -n "$home" && -f "$home/accel_pipeline_mcp.env" ]]; then
    # Only what codex did not pass: the sidecar is shared by every pipeline using this
    # CODEX_HOME (the last step written wins), so the step's own env from codex
    # (-c mcp_servers.accel_pipeline.env: query, stage, context, ...) always takes precedence.
    while IFS= read -r line || [[ -n "$line" ]]; do
      [[ "$line" =~ ^([A-Za-z_][A-Za-z0-9_]*)= ]] || continue
      [[ -z "${!BASH_REMATCH[1]+x}" ]] && eval "export $line"
    done < "$home/accel_pipeline_mcp.env"
    break
  fi
done

if [[ -x "$ORCH/.venv/bin/python" ]]; then
  PY="$ORCH/.venv/bin/python"
elif command -v python3 >/dev/null 2>&1; then
  PY="$(command -v python3)"
else
  echo "accel_pipeline MCP: no python found (expected $ORCH/.venv)" >&2
  exit 1
fi

export PYTHONPATH="${ORCH}${PYTHONPATH:+:$PYTHONPATH}"
cd "$ORCH"
exec "$PY" -m accel_pipeline.mcp_server
