from __future__ import annotations

from accel_pipeline.backends.base import AgentBackend, StageRunRequest, StageRunResult
from accel_pipeline.backends.cursor_sdk import CursorSDKBackend
from accel_pipeline.backends.openai_agents import OpenAIAgentsBackend
from accel_pipeline.config import ModelConfig, PipelineConfig
from accel_pipeline.tools.tool_policy import build_stage_tool_registry, build_tools


def create_agent_backend(
    *,
    provider: str,
    pipeline_config: PipelineConfig,
    query: int,
    stage: str,
    model_config: ModelConfig,
) -> AgentBackend:
    if provider == "cursor":
        registry = build_stage_tool_registry(pipeline_config, query, stage)
        return CursorSDKBackend(
            repo_root=pipeline_config.repo_root,
            api_key_env=model_config.api_key_env,
            tool_registry=registry,
        )
    if provider in {"openai", "openrouter"}:
        tools = build_tools(pipeline_config, query, stage)
        return OpenAIAgentsBackend(tools=tools)
    raise ValueError(f"unsupported provider: {provider!r}")


__all__ = [
    "AgentBackend",
    "CursorSDKBackend",
    "OpenAIAgentsBackend",
    "StageRunRequest",
    "StageRunResult",
    "create_agent_backend",
]
