from __future__ import annotations

import yaml

from accel_pipeline.config import PipelineConfig, load_workload


def workload_post_implementation_gates(workload: dict) -> list[str]:
    if workload.get("workload_kind") == "aux_state":
        return list(workload.get("post_implementation_gates", ["aux_correctness", "aux_unit"]))
    return ["correctness", "baseline"]


def workload_post_optimization_gates(workload: dict) -> list[str]:
    if workload.get("workload_kind") == "aux_state":
        gates = list(workload.get("post_optimization_gates", ["aux_correctness", "aux_unit"]))
        return gates
    return ["correctness", "baseline", "sf10", "profile"]


def workload_post_audit_gates(workload: dict) -> list[str]:
    if workload.get("workload_kind") == "aux_state":
        return list(workload.get("post_audit_gates", ["aux_correctness", "aux_unit", "fairness"]))
    return ["fairness", "sf10"]


def run_post_stage_gates(
    pipeline_config: PipelineConfig,
    workload_name: str,
    query: int,
    gate_names: list[str],
) -> None:
    from accel_pipeline.gates import run_gate

    workload = load_workload(workload_name, pipeline_config.repo_root)
    for gate_name in gate_names:
        run_gate(
            pipeline_config,
            gate_name,
            query,
            workload=workload,
            raise_on_failure=True,
        )
