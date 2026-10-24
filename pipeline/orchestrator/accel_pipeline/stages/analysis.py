from __future__ import annotations

import json
import sys

from accel_pipeline.config import ModelConfig, PipelineConfig, load_run_workload
from accel_pipeline.paths import analysis_artifact_path
from accel_pipeline.pipeline_log import log_analysis_candidates
from accel_pipeline.stages.base import run_agent_stage
from accel_pipeline.validation import AuxAnalysisValidationError, repair_aux_analysis, validate_aux_analysis_file


def _ensure_dataset(pipeline_config: PipelineConfig, workload_name: str) -> None:
    """The data tools query the target engine: bring its instance up with the dataset loaded before the agent starts."""
    from accel_pipeline.engine_host import ensure_dataset_loaded

    workload = load_run_workload(pipeline_config, workload_name)
    if workload.get("workload_kind") != "aux_state":
        return
    problem = ensure_dataset_loaded(workload, pipeline_config.repo_root, pipeline_config.build_dir)
    if problem:
        print(f"[pipeline] data tools unavailable: {problem}", file=sys.stderr, flush=True)


def run(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
    *,
    user_message: str | None = None,
    analysis_revision_request: str = "",
):
    if not dry_run:
        _ensure_dataset(pipeline_config, workload_name)
    result = run_agent_stage(
        stage="analysis",
        prompt_file="01_analysis.md",
        pipeline_config=pipeline_config,
        model_config=model_config,
        workload_name=workload_name,
        query=query,
        user_message=user_message
        or "Analyze the query and write the analysis artifact to the artifacts directory.",
        model_overrides=model_overrides,
        dry_run=dry_run,
        analysis_revision_request=analysis_revision_request,
    )
    if dry_run:
        return result

    workload = load_run_workload(pipeline_config, workload_name)
    if workload.get("workload_kind") != "aux_state":
        return result

    analysis_path = analysis_artifact_path(pipeline_config.repo_root, query, workload=workload)
    allow_poor = bool(workload.get("allow_poor_fitness", False))
    try:
        analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
        repaired, repairs = repair_aux_analysis(analysis)
        if repairs:
            analysis_path.write_text(json.dumps(repaired, indent=2), encoding="utf-8")
            print(
                f"[pipeline] repaired analysis.json before validation: {', '.join(repairs)}",
                file=sys.stderr,
                flush=True,
            )
        validate_aux_analysis_file(
            analysis_path,
            allow_poor_fitness=allow_poor,
        )
    except AuxAnalysisValidationError as exc:
        raise RuntimeError(f"aux analysis validation failed: {exc}") from exc

    analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
    log_analysis_candidates(
        pipeline_config.repo_root,
        query,
        analysis,
        revision=bool(analysis_revision_request),
        workload=workload,
    )
    return result
