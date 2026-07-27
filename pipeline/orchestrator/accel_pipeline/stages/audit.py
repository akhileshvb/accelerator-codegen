from __future__ import annotations

from accel_pipeline.config import ModelConfig, PipelineConfig, load_workload
from accel_pipeline.stages.gate_plan import run_post_stage_gates, workload_post_audit_gates
from accel_pipeline.stages.base import run_agent_stage


def run(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
):
    result = run_agent_stage(
        stage="audit",
        prompt_file="04_audit.md",
        pipeline_config=pipeline_config,
        model_config=model_config,
        workload_name=workload_name,
        query=query,
        user_message="Produce final_audit.md after fairness and scale review.",
        model_overrides=model_overrides,
        dry_run=dry_run,
    )
    if not dry_run:
        run_post_stage_gates(
            pipeline_config,
            workload_name,
            query,
            workload_post_audit_gates(load_workload(workload_name, pipeline_config.repo_root)),
        )
    return result
