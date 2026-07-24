from __future__ import annotations

from accel_pipeline.config import ModelConfig, PipelineConfig
from accel_pipeline.stages.base import run_agent_stage


def run(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
):
    return run_agent_stage(
        stage="analysis",
        prompt_file="01_analysis.md",
        pipeline_config=pipeline_config,
        model_config=model_config,
        workload_name=workload_name,
        query=query,
        user_message="Analyze the query and write analysis.json to the artifacts directory.",
        model_overrides=model_overrides,
        dry_run=dry_run,
    )
