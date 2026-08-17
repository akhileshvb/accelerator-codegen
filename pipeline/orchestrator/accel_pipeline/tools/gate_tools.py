from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any

from accel_pipeline.config import PipelineConfig
from accel_pipeline.gates import run_gate


@dataclass
class ToolContext:
    pipeline_config: PipelineConfig
    query: int
    workload: dict[str, Any] | None = None


def _compact_gate_payload(gate_name: str, data: dict) -> dict:
    compact: dict = {"gate": gate_name, "status": data.get("status")}
    for key in (
        "composite_score",
        "composite_workload_speedup",
        "read_only_query_speedup",
        "read_only_cold_start_workload_speedup",
        "exit_code",
        "reason",
        "notes",
        "smoke_exit_code",
        "differential_exit_code",
    ):
        if key in data and data[key] is not None:
            compact[key] = data[key]
    if data.get("status") == "fail":
        tail = str(data.get("log_tail") or data.get("notes") or "").strip()
        if tail:
            # Agents need enough failure text to fix impl mid-stage.
            compact["failure_summary"] = tail[-4000:]
    return compact


def make_gate_tools(ctx: ToolContext):
    def request_gate(gate_name: str) -> str:
        """Run an official orchestrator gate; returns JSON status (does not raise on fail).

        Implementation mid-loop: call with ``aux_correctness`` / ``aux_multi_connection``
        (or use MCP ``run_correctness`` / ``run_multi_connection``) and keep fixing until
        ``status`` is ``pass``.
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

    return request_gate
