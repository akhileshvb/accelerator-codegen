from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any

from accel_pipeline.config import PipelineConfig
from accel_pipeline.gates import run_gate

ENGINE_LAYER_GATES = (
    "engine_build",
    "engine_store",
    "engine_aux_backend",
    "engine_txn",
    "engine_mutations",
    "engine_scan",
    "engine_runtime",
    "engine_visibility",
    "engine_host",
    "engine_adversarial",
)


def layer_gate_from_stage(stage: str) -> str | None:
    """Map ``impl_txn`` → ``engine_txn``. None when this is not a layer impl stage."""
    if not str(stage or "").startswith("impl_"):
        return None
    lid = str(stage)[len("impl_") :].strip()
    if not lid:
        return None
    return f"engine_{lid}"


@dataclass
class ToolContext:
    pipeline_config: PipelineConfig
    query: int
    workload: dict[str, Any] | None = None
    stage: str = ""


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
        "smoke_exit_code",
        "differential_exit_code",
    ):
        if key in data and data[key] is not None:
            compact[key] = data[key]
    if data.get("status") == "fail":
        from accel_pipeline.gate_capture import log_tail_for_agent

        tail = log_tail_for_agent(data, max_chars=4000)
        compact["failure_summary"] = tail
    for key in ("layer", "cmake_target", "harness", "mode", "engine"):
        if data.get(key):
            compact[key] = data[key]
    return compact


def make_gate_tools(ctx: ToolContext):
    def request_gate(gate_name: str) -> str:
        """MCP TOOL (not a resource). Run an official orchestrator gate by name.

        Engine-backend layers: ``engine_build`` then this layer's gate
        (``engine_store``, ``engine_aux_backend``, ``engine_txn``, …) after each
        increment until ``status`` is ``pass``. Prefer ``run_engine_build`` /
        ``run_layer_gate`` when listed. Docker hosts: this is the in-container
        build+run — do not cmake on the developer machine.
        Aux: ``aux_correctness`` / ``aux_multi_connection``.
        """
        result = run_gate(
            ctx.pipeline_config,
            gate_name,
            ctx.query,
            workload=ctx.workload,
            raise_on_failure=False,
            allow_skip=True,
        )
        return json.dumps(_compact_gate_payload(result.name, result.data), indent=2)

    def run_engine_build() -> str:
        """MCP TOOL. Official ``engine_build`` gate (host cmake or docker ensure+build)."""
        return request_gate("engine_build")

    def run_layer_gate() -> str:
        """MCP TOOL. Official gate for this impl_* layer (e.g. impl_txn → engine_txn)."""
        gate = layer_gate_from_stage(ctx.stage)
        if not gate:
            return json.dumps(
                {
                    "error": "run_layer_gate is for engine_backend impl_* stages",
                    "stage": ctx.stage,
                    "hint": (
                        "Call request_gate with engine_store / engine_txn / … "
                        "or run_engine_build()."
                    ),
                },
                indent=2,
            )
        return request_gate(gate)

    return request_gate, run_engine_build, run_layer_gate
