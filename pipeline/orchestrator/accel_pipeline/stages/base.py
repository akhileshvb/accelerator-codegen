from __future__ import annotations

from typing import Any

from accel_pipeline.config import ModelConfig, PipelineConfig, load_workload
from accel_pipeline.models import make_model_id
from accel_pipeline.prompts import render_prompt, save_rendered_prompt
from accel_pipeline.tools.tool_policy import build_tools


def _wrap_tools(tools: list) -> list:
    from agents import function_tool

    return [function_tool(fn) for fn in tools]


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

    from agents import Agent, Runner

    model_id = make_model_id(stage, pipeline_config, model_config, model_overrides)
    tools = _wrap_tools(build_tools(pipeline_config, query, stage))
    agent = Agent(
        name=f"accel_{stage}",
        model=model_id,
        instructions=prompt,
        tools=tools,
    )
    result = Runner.run_sync(agent, user_message)
    # TODO: SQLiteSession per stage for multi-turn compile-fix loops (cap at stage_max_turns).
    return result
