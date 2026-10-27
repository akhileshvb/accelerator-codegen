from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any

from accel_pipeline.gates.parallel import background_status, start_background_gate
from accel_pipeline.config import PipelineConfig
from accel_pipeline.gates.run import run_gate


@dataclass
class ToolContext:
    pipeline_config: PipelineConfig
    query: int
    workload: dict[str, Any] | None = None
    stage: str = ""
    workload_name: str = ""
    optimization_iter: int = 0
    writes: Any = None  # the session's StageWriteCheck: no check runs on a tree with out-of-bounds edits


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
        "failed_phases",
        "phases",
        "stacks",
    ):
        if key in data and data[key] is not None:
            compact[key] = data[key]
    if waited := (data.get("phase_timings") or {}).get("measurement_lease_wait"):
        compact["measurement_lease"] = f"waited {waited} s for the measurement lease"
    if data.get("status") == "fail":
        from accel_pipeline.gates.run import log_tail_for_agent

        compact["failure_summary"] = log_tail_for_agent(data, max_chars=4000)
    for key in ("layer", "cmake_target", "harness", "mode", "engine"):
        if data.get(key):
            compact[key] = data[key]
    return compact


def _compact_background_state(state: dict) -> dict:
    compact = {k: state[k] for k in ("gate", "status", "reason", "note", "elapsed_sec", "seconds", "phase", "progress", "stale") if k in state}
    if state.get("data"):
        compact.update({k: v for k, v in _compact_gate_payload(state["gate"], state["data"]).items() if k != "gate"})
    return compact


def admission_refusal_payload(ctx: ToolContext, gate_name: str) -> str:
    """The refusal (JSON) when `gate_name` may not run yet, "" when it may: the session edited paths its stage may
    not write, or a heavy gate has no passing quick_check on this code."""
    from accel_pipeline.quick import admission_refusal
    from accel_pipeline.layout import violation_message

    if ctx.writes is not None and (bad := ctx.writes.violations()):
        reason = f"{gate_name} refused: {violation_message(ctx.stage, bad)}"
    else:
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
        Aux: ``aux_exit`` (every heavy check in one run, a few minutes) or one of
        ``aux_correctness`` / ``aux_multi_connection``. Gates are for acceptance; for the
        edit-test loop use ``quick_check``.

        ``background=True`` starts the gate in its own process and returns at once;
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

    return request_gate, gate_status
