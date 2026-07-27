from __future__ import annotations

from pathlib import Path

from accel_pipeline.config import ModelConfig, PipelineConfig, load_workload
from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.stages.base import run_agent_stage
from accel_pipeline.validation import AuxAnalysisValidationError, validate_aux_analysis_file


def run(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
):
    result = run_agent_stage(
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
    if dry_run:
        return result

    workload = load_workload(workload_name, pipeline_config.repo_root)
    if workload.get("workload_kind") != "aux_state":
        return result

    analysis_path = query_artifacts_dir(pipeline_config.repo_root, query) / "analysis.json"
    try:
        validate_aux_analysis_file(
            analysis_path,
            allow_poor_fitness=bool(workload.get("allow_poor_fitness", False)),
        )
    except AuxAnalysisValidationError as exc:
        raise RuntimeError(f"aux analysis validation failed: {exc}") from exc
    return result
