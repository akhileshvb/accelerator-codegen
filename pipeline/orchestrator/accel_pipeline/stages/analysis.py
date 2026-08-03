from __future__ import annotations

import json
from pathlib import Path

from accel_pipeline.config import ModelConfig, PipelineConfig, load_workload
from accel_pipeline.paths import pipeline_root, query_artifacts_dir
from accel_pipeline.pipeline_log import log_analysis_candidates
from accel_pipeline.stages.base import run_agent_stage
from accel_pipeline.validation import AuxAnalysisValidationError, validate_aux_analysis_file


def _legacy_analysis_path(repo_root: Path, query: int) -> Path:
    return pipeline_root(repo_root) / "artifacts" / f"q{query}" / "analysis.json"


def _analysis_is_valid(path: Path, *, allow_poor_fitness: bool) -> bool:
    if not path.is_file():
        return False
    try:
        validate_aux_analysis_file(path, allow_poor_fitness=allow_poor_fitness)
    except AuxAnalysisValidationError:
        return False
    return True


def _maybe_recover_legacy_analysis(
    repo_root: Path,
    query: int,
    analysis_path: Path,
    *,
    allow_poor_fitness: bool,
) -> bool:
    """Copy valid aux analysis from legacy tpch-style path (pipeline/artifacts/qN/)."""
    legacy_path = _legacy_analysis_path(repo_root, query)
    if legacy_path.resolve() == analysis_path.resolve():
        return False
    if not _analysis_is_valid(legacy_path, allow_poor_fitness=allow_poor_fitness):
        return False
    analysis_path.parent.mkdir(parents=True, exist_ok=True)
    analysis_path.write_text(legacy_path.read_text(encoding="utf-8"), encoding="utf-8")
    return True


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
    result = run_agent_stage(
        stage="analysis",
        prompt_file="01_analysis.md",
        pipeline_config=pipeline_config,
        model_config=model_config,
        workload_name=workload_name,
        query=query,
        user_message=user_message
        or "Analyze the query and write analysis.json to the artifacts directory.",
        model_overrides=model_overrides,
        dry_run=dry_run,
        analysis_revision_request=analysis_revision_request,
    )
    if dry_run:
        return result

    workload = load_workload(workload_name, pipeline_config.repo_root)
    if workload.get("workload_kind") != "aux_state":
        return result

    analysis_path = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload) / "analysis.json"
    allow_poor = bool(workload.get("allow_poor_fitness", False))
    if not _analysis_is_valid(analysis_path, allow_poor_fitness=allow_poor):
        _maybe_recover_legacy_analysis(
            pipeline_config.repo_root,
            query,
            analysis_path,
            allow_poor_fitness=allow_poor,
        )
    try:
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
    )
    return result
