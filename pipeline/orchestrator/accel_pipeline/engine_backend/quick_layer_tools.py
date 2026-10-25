"""quick_layer: the inner loop of an engine_backend layer in seconds (incremental build of the
layer's target, one harness run, the canary checks the layer owns), with compact output."""

from __future__ import annotations

import json
import os
from typing import Any, Callable

from accel_pipeline.gates.run import run_gate
from accel_pipeline.tools.gate_tools import ToolContext

_FAILURE_TAIL_CHARS = 2000


def layer_id_from_stage(stage: str) -> str:
    """`impl_epochs` -> `epochs`; "" when this is not a layer impl stage."""
    return stage[len("impl_") :].strip() if stage.startswith("impl_") else ""


def make_quick_layer_tool(ctx: ToolContext) -> Callable[[str], str]:
    def quick_layer(layer: str = "") -> str:
        """MCP TOOL. Fast inner loop for one engine_backend layer, in seconds: an incremental
        `host.sh build` of the layer's target, ONE run of its harness (the layer gate repeats it
        to catch races), and, once the backend has its canary binding, the canary checks this
        layer owns. Output: pass/fail, seconds per phase, and on failure the failing output.
        Call it after every edit; `run_layer_gate` / `request_gate` are for the final check.
        `layer`: a layer id; empty means the layer of this stage.
        """
        layer_id = layer or layer_id_from_stage(ctx.stage)
        if not layer_id:
            return json.dumps({"error": "no layer: pass a layer id (hostctl, capture, ...)"})
        previous = os.environ.get("ACCEL_QUICK_LAYER")
        os.environ["ACCEL_QUICK_LAYER"] = layer_id
        try:
            result = run_gate(ctx.pipeline_config, "engine_quick", ctx.query, workload=ctx.workload,
                              raise_on_failure=False, allow_skip=True)
        finally:
            if previous is None:
                os.environ.pop("ACCEL_QUICK_LAYER", None)
            else:
                os.environ["ACCEL_QUICK_LAYER"] = previous
        return json.dumps(_compact(layer_id, result.status, result.data), indent=2)

    return quick_layer


def _compact(layer_id: str, status: str, data: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {"layer": layer_id, "status": status, "phase_seconds": data.get("phase_seconds") or {}}
    if status != "pass":
        out["reason"] = data.get("reason")
        out["failure_summary"] = str(data.get("log_tail") or "")[-_FAILURE_TAIL_CHARS:]
    return out
