"""MCP tools of an engine_backend run: the engine build, the layer gate and the layer's inner loop."""

from __future__ import annotations

import json
import os
from typing import Any, Callable

from accel_pipeline.gates.run import run_gate
from accel_pipeline.tools.gate_tools import ToolContext, make_gate_tools

_FAILURE_TAIL_CHARS = 2000


def layer_gate_from_stage(stage: str, workload: dict[str, Any] | None = None) -> str | None:
    """Map ``impl_epochs`` to the gate of that layer in the workload. None when this is not a layer impl stage."""
    if not str(stage or "").startswith("impl_"):
        return None
    lid = str(stage)[len("impl_") :].strip()
    if not lid:
        return None
    for layer in (workload or {}).get("implementation_layers") or []:
        if str(layer.get("id") or "") == lid:
            return str(layer.get("gate") or "") or None
    return f"engine_{lid}"


def layer_id_from_stage(stage: str) -> str:
    """`impl_epochs` -> `epochs`; "" when this is not a layer impl stage."""
    return stage[len("impl_") :].strip() if stage.startswith("impl_") else ""


def callables(ctx: ToolContext) -> dict[str, Callable[..., str]]:
    request_gate, _ = make_gate_tools(ctx)

    def run_engine_build() -> str:
        """MCP TOOL. Official ``engine_build`` gate (host cmake or docker ensure+build)."""
        return request_gate("engine_build")

    def run_layer_gate() -> str:
        """MCP TOOL. Official gate for this impl_* layer (e.g. impl_epochs → engine_epochs)."""
        gate = layer_gate_from_stage(ctx.stage, ctx.workload)
        if not gate:
            return json.dumps(
                {
                    "error": "run_layer_gate is for engine_backend impl_* stages",
                    "stage": ctx.stage,
                    "hint": "Call request_gate with engine_capture / engine_epochs / … or run_engine_build().",
                },
                indent=2,
            )
        return request_gate(gate)

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

    return {"run_engine_build": run_engine_build, "run_layer_gate": run_layer_gate, "quick_layer": quick_layer}


def _compact(layer_id: str, status: str, data: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {"layer": layer_id, "status": status, "phase_seconds": data.get("phase_seconds") or {}}
    if status != "pass":
        out["reason"] = data.get("reason")
        out["failure_summary"] = str(data.get("log_tail") or "")[-_FAILURE_TAIL_CHARS:]
    return out


def register(tool, call) -> None:
    @tool(read_only=False)
    def run_engine_build() -> str:
        """MCP TOOL. Official engine_build gate (docker ensure+build or host cmake).

        Call this after CMake/adapter changes. On fail, read failure_summary, fix, retry.
        """
        return call("run_engine_build")

    @tool(read_only=False)
    def run_layer_gate() -> str:
        """MCP TOOL. Official harness gate for this impl_* layer.

        Maps impl_capture→engine_capture, impl_epochs→engine_epochs, etc. Call after
        run_engine_build. On fail, read failure_summary, fix, retry until pass.
        """
        return call("run_layer_gate")

    @tool(read_only=False)
    def quick_layer(layer: str = "") -> str:
        """MCP TOOL. Seconds-long inner loop of an engine_backend layer: incremental build, one harness run, the canary checks the layer owns."""
        return call("quick_layer", layer)
