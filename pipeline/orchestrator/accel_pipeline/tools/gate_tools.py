from __future__ import annotations

import json
from dataclasses import dataclass

from accel_pipeline.config import PipelineConfig
from accel_pipeline.gates import run_gate


@dataclass
class ToolContext:
    pipeline_config: PipelineConfig
    query: int


def make_gate_tools(ctx: ToolContext):
    def request_gate(gate_name: str) -> str:
        """Run an official orchestrator gate (correctness, baseline, sf10, profile, fairness)."""
        result = run_gate(ctx.pipeline_config, gate_name, ctx.query)
        return json.dumps({"gate": result.name, "status": result.status, "data": result.data})

    return request_gate
