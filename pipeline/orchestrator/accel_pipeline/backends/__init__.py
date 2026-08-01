from __future__ import annotations

from accel_pipeline.backends.base import AgentBackend, StageRunRequest, StageRunResult
from accel_pipeline.backends.cursor_sdk import CursorSDKBackend
from accel_pipeline.backends.openai_agents import OpenAIAgentsBackend
from accel_pipeline.config import ModelConfig, PipelineConfig, load_workload
from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.tools.tool_policy import build_stage_tool_registry, build_tools


def create_agent_backend(
    *,
    provider: str,
    pipeline_config: PipelineConfig,
    query: int,
    stage: str,
    model_config: ModelConfig,
    workload_name: str = "",
    optimization_iter: int = 0,
) -> AgentBackend:
    if provider == "cursor":
        registry = build_stage_tool_registry(
            pipeline_config,
            query,
            stage,
            workload_name=workload_name,
            optimization_iter=optimization_iter,
        )
        art_dir = None
        compress_outputs = stage in ("implementation", "optimization", "crash_debug", "audit")
        if compress_outputs and workload_name:
            workload = load_workload(workload_name, pipeline_config.repo_root)
            art_dir = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
        return CursorSDKBackend(
            repo_root=pipeline_config.repo_root,
            api_key_env=model_config.api_key_env,
            tool_registry=registry,
            art_dir=art_dir,
            compress_outputs=compress_outputs,
        )
    if provider in {"openai", "openrouter"}:
        tools = build_tools(
            pipeline_config,
            query,
            stage,
            workload_name=workload_name,
            optimization_iter=optimization_iter,
        )
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
