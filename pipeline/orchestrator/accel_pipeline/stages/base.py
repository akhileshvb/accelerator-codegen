from __future__ import annotations

from typing import Any

from accel_pipeline.backends import StageRunRequest, create_agent_backend
from accel_pipeline.config import ModelConfig, PipelineConfig, load_workload, apply_workload_overrides
from accel_pipeline.models import make_model_id
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
) -> Any:
    workload = load_workload(workload_name, pipeline_config.repo_root)
    pipeline_config = apply_workload_overrides(pipeline_config, workload)
    prompt = render_prompt(
        pipeline_config,
        prompt_file,
        workload,
        query,
        stage,
        optimization_iter=optimization_iter,
    )
    save_rendered_prompt(
        pipeline_config,
        query,
        stage,
        prompt,
        optimization_iter=optimization_iter,
    )

    if dry_run:
        return {"dry_run": True, "stage": stage, "prompt_chars": len(prompt)}

    model_id = make_model_id(stage, pipeline_config, model_config, model_overrides)
    backend = create_agent_backend(
        provider=model_config.provider,
        pipeline_config=pipeline_config,
        query=query,
        stage=stage,
        model_config=model_config,
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
        raise RuntimeError(
            f"{backend.name} stage {stage} failed: {result.error or result.status}"
        )
    return result
