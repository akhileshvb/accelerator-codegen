from __future__ import annotations

from typing import Any

from accel_pipeline.backends import StageRunRequest, create_agent_backend
from accel_pipeline.config import ModelConfig, PipelineConfig, load_workload, apply_workload_overrides
from accel_pipeline.models import make_model_id
from accel_pipeline.pipeline_log import (
    log_stage_complete,
    log_stage_start,
    usage_from_metadata,
)
from accel_pipeline.prompts import render_prompt, save_rendered_prompt
from accel_pipeline.tools.tool_policy import TOOL_USAGE_INSTRUCTIONS


def run_agent_stage(
    *,
    stage: str,
    prompt_file: str,
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    user_message: str,
    model_overrides: dict[str, str] | None = None,
    optimization_iter: int = 0,
    dry_run: bool = False,
    analysis_revision_request: str = "",
) -> Any:
    workload = load_workload(workload_name, pipeline_config.repo_root)
    pipeline_config = apply_workload_overrides(pipeline_config, workload)
    extra_context = (
        {"analysis_revision_request": analysis_revision_request}
        if analysis_revision_request
        else None
    )
    prompt = render_prompt(
        pipeline_config,
        prompt_file,
        workload,
        query,
        stage,
        optimization_iter=optimization_iter,
        extra_context=extra_context or None,
    )
    save_rendered_prompt(
        pipeline_config,
        query,
        stage,
        prompt,
        optimization_iter=optimization_iter,
    )

    if dry_run:
        log_stage_complete(
            pipeline_config.repo_root,
            query,
            stage=stage,
            model_id="(dry-run)",
            backend="dry-run",
            duration_ms=None,
            usage=None,
            optimization_iter=optimization_iter,
            dry_run=True,
            prompt_chars=len(prompt),
        )
        return {"dry_run": True, "stage": stage, "prompt_chars": len(prompt)}

    model_id = make_model_id(stage, pipeline_config, model_config, model_overrides)
    log_stage_start(
        pipeline_config.repo_root,
        query,
        stage=stage,
        model_id=model_id,
        optimization_iter=optimization_iter,
        revision=bool(analysis_revision_request),
    )
    backend = create_agent_backend(
        provider=model_config.provider,
        pipeline_config=pipeline_config,
        query=query,
        stage=stage,
        model_config=model_config,
        workload_name=workload_name,
        optimization_iter=optimization_iter,
    )
    max_turns = int(pipeline_config.stage_max_turns.get(stage, 15))
    instructions = prompt
    if model_config.provider != "cursor":
        instructions = f"{prompt.rstrip()}\n\n{TOOL_USAGE_INSTRUCTIONS}"

    request = StageRunRequest(
        stage=stage,
        instructions=instructions,
        user_message=user_message,
        agent_name=f"accel_{stage}",
        model_id=model_id,
        max_turns=max_turns,
    )
    result = backend.run_stage(request)
    if result.status != "finished":
        detail = (result.error or result.status or "").strip()
        final = (result.final_text or "").strip()
        if final and final not in detail:
            clipped = final if len(final) <= 3000 else final[:3000] + "\n... [truncated]"
            detail = f"{detail}\n\n--- agent output ---\n{clipped}"
        raise RuntimeError(f"{backend.name} stage {stage} failed: {detail}")
    log_stage_complete(
        pipeline_config.repo_root,
        query,
        stage=stage,
        model_id=model_id,
        backend=result.backend,
        duration_ms=result.metadata.get("duration_ms"),
        usage=usage_from_metadata(result.metadata),
        optimization_iter=optimization_iter,
        run_id=result.run_id,
    )
    return result
