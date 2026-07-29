from __future__ import annotations

import json
from dataclasses import dataclass

from accel_pipeline.config import PipelineConfig
from accel_pipeline.gates import run_gate


@dataclass
class ToolContext:
    pipeline_config: PipelineConfig
    query: int


def _compact_gate_payload(gate_name: str, data: dict) -> dict:
    compact: dict = {"gate": gate_name, "status": data.get("status")}
    for key in (
        "composite_score",
        "composite_workload_speedup",
        "read_only_query_speedup",
        "exit_code",
        "reason",
        "notes",
    ):
        if key in data and data[key] is not None:
            compact[key] = data[key]
    if data.get("status") == "fail":
        tail = str(data.get("log_tail") or data.get("notes") or "").strip()
        if tail:
            compact["failure_summary"] = tail[-500:]
    return compact


def make_gate_tools(ctx: ToolContext):
    def request_gate(gate_name: str) -> str:
        """Run an official orchestrator gate (audit speedup verification, etc.)."""
        result = run_gate(
            ctx.pipeline_config,
            gate_name,
            ctx.query,
            raise_on_failure=True,
        )
        return json.dumps(_compact_gate_payload(result.name, result.data), separators=(",", ":"))

    return request_gate
