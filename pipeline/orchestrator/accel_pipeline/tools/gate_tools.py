from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any

from accel_pipeline.background_gates import background_status, start_background_gate
from accel_pipeline.config import PipelineConfig
from accel_pipeline.gates import run_gate


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


@dataclass
class ToolContext:
    pipeline_config: PipelineConfig
    query: int
    workload: dict[str, Any] | None = None
    stage: str = ""
    workload_name: str = ""


def _compact_gate_payload(gate_name: str, data: dict) -> dict:
    compact: dict = {"gate": gate_name, "status": data.get("status")}
    for key in (
        "composite_score",
        "composite_workload_speedup",
        "read_only_query_speedup",
        "read_only_cold_start_workload_speedup",
        "exit_code",
        "returncode",
        "reason",
        "notes",
        "reproduce",
        "smoke_exit_code",
        "differential_exit_code",
    ):
        if key in data and data[key] is not None:
            compact[key] = data[key]
    if waited := (data.get("phase_timings") or {}).get("measurement_lease_wait"):
        compact["measurement_lease"] = f"waited {waited} s for the measurement lease"
    if data.get("status") == "fail":
        from accel_pipeline.gate_capture import log_tail_for_agent

        tail = log_tail_for_agent(data, max_chars=4000)
        compact["failure_summary"] = tail
    for key in ("layer", "cmake_target", "harness", "mode", "engine"):
        if data.get(key):
            compact[key] = data[key]
    return compact


def _compact_background_state(state: dict) -> dict:
    compact = {k: state[k] for k in ("gate", "status", "reason", "note", "elapsed_sec", "stale") if k in state}
    if state.get("data"):
        compact.update({k: v for k, v in _compact_gate_payload(state["gate"], state["data"]).items() if k != "gate"})
    return compact


def admission_refusal_payload(ctx: ToolContext, gate_name: str) -> str:
    """The refusal (JSON) when `gate_name` may not run before a passing quick_check on this code; "" when it may."""
    from accel_pipeline.quick import admission_refusal

    reason = admission_refusal(ctx.pipeline_config, ctx.workload or {}, ctx.query, gate_name)
    return json.dumps({"gate": gate_name, "status": "refused", "reason": reason}, indent=2) if reason else ""


def make_gate_tools(ctx: ToolContext):
    def request_gate(gate_name: str, background: bool = False) -> str:
        """MCP TOOL (not a resource). Run an official orchestrator gate by name.

        Engine-backend layers: ``engine_build`` then this layer's gate
        (``engine_capture``, ``engine_epochs``, …) after each
        increment until ``status`` is ``pass``. Prefer ``run_engine_build`` /
        ``run_layer_gate`` when listed. Docker hosts: this is the in-container
        build+run — do not cmake on the developer machine.
        Aux: ``aux_correctness`` / ``aux_multi_connection``. Gates are for acceptance and take
        minutes; for the edit-test loop use ``quick_check``.

        ``background=True`` starts the gate in its own process and returns at once (only
        aux_correctness, aux_unit, prims_tests, aux_multi_connection and aux_cross_accel);
        keep working and read the result once with ``gate_status``. Never poll it in a loop.
        """
        if refusal := admission_refusal_payload(ctx, gate_name):
            return refusal
        if background:
            state = start_background_gate(
                ctx.pipeline_config, ctx.workload or {}, ctx.query, gate_name, workload_name=ctx.workload_name
            )
            return json.dumps(_compact_background_state(state), indent=2)
        result = run_gate(
            ctx.pipeline_config,
            gate_name,
            ctx.query,
            workload=ctx.workload,
            raise_on_failure=False,
            allow_skip=True,
        )
        return json.dumps(_compact_gate_payload(result.name, result.data), indent=2)

    def gate_status(gate_name: str) -> str:
        """MCP TOOL. State of the background run of a gate: running, or its result.

        ``stale: true`` means the code changed after it ran: its result does not cover your latest
        edit, and the orchestrator reruns the gate at stage exit. Call it once when you need the
        answer, not in a loop.
        """
        state = background_status(ctx.pipeline_config, ctx.workload or {}, ctx.query, gate_name)
        return json.dumps(_compact_background_state(state), indent=2)

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
                    "hint": (
                        "Call request_gate with engine_capture / engine_epochs / … "
                        "or run_engine_build()."
                    ),
                },
                indent=2,
            )
        return request_gate(gate)

    return request_gate, run_engine_build, run_layer_gate, gate_status
