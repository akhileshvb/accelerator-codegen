from __future__ import annotations

from typing import Any

from accel_pipeline.backends import StageRunRequest, create_agent_backend
from accel_pipeline.config import ModelConfig, PipelineConfig, load_workload, apply_workload_overrides
from accel_pipeline.models import make_model_id, resolve_agent_model
from accel_pipeline.agent_progress import begin_agent_run
from accel_pipeline.pipeline_log import (
    log_stage_complete,
    log_stage_start,
    usage_from_metadata,
)
from accel_pipeline.prompts import render_prompt, save_rendered_prompt
from accel_pipeline.tools.tool_policy import STAGE_TOOL_NAMES, tool_usage_instructions

_UNLIMITED_STAGE_TURNS = 10_000


def resolve_stage_max_turns(stage_max_turns: dict[str, int], stage: str, default: int = 15) -> int:
    raw = int(stage_max_turns.get(stage, default))
    if raw <= 0:
        return _UNLIMITED_STAGE_TURNS
    return raw


def resolve_optimization_max_turns(pipeline_config: Any, phase: str) -> int:
    """Resolve optimization sub-stage turns; 0 or unset means unlimited."""
    key = f"optimization_max_turns_{phase}"
    raw = int(getattr(pipeline_config, key, 0) or 0)
    if raw <= 0:
        return _UNLIMITED_STAGE_TURNS
    return raw


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
    agent_model = resolve_agent_model(stage, pipeline_config, model_config, model_overrides)
    log_stage_start(
        pipeline_config.repo_root,
        query,
        stage=stage,
        model_id=model_id,
        optimization_iter=optimization_iter,
        revision=bool(analysis_revision_request),
    )
    from accel_pipeline.models import resolve_stage_provider

    stage_provider = resolve_stage_provider(stage, model_config)
    backend = create_agent_backend(
        provider=stage_provider,
        pipeline_config=pipeline_config,
        query=query,
        stage=stage,
        model_config=model_config,
        workload_name=workload_name,
        optimization_iter=optimization_iter,
    )
    max_turns = resolve_stage_max_turns(pipeline_config.stage_max_turns, stage)
    instructions = prompt
    if stage_provider not in {"cursor", "codex_exec"}:
        instructions = f"{prompt.rstrip()}\n\n{tool_usage_instructions(stage)}"
    elif stage_provider == "codex_exec":
        from pathlib import Path

        codex_doc = pipeline_config.pipeline_root / "prompts" / "shared" / "codex_tooling.md"
        if codex_doc.is_file() and "codex_tooling.md" not in instructions:
            instructions = f"{prompt.rstrip()}\n\n{codex_doc.read_text(encoding='utf-8')}"

    begin_agent_run(
        stage,
        max_turns=max_turns,
        tools=STAGE_TOOL_NAMES.get(stage, []),
        prompt_chars=len(instructions),
    )

    request = StageRunRequest(
        stage=stage,
        instructions=instructions,
        user_message=user_message,
        agent_name=f"accel_{stage}",
        model_id=model_id,
        agent_model=agent_model,
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
