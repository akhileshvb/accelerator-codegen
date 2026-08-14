from __future__ import annotations

from typing import Any

from accel_pipeline.backends.base import AgentBackend, StageRunRequest, StageRunResult
from accel_pipeline.backends.cursor_sdk import CursorSDKBackend
from accel_pipeline.backends.openai_agents import build_openai_agents_backend
from accel_pipeline.config import ModelConfig, PipelineConfig, load_workload
from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.tools.tool_policy import STAGE_TOOL_NAMES, build_stage_tool_registry


def create_agent_backend(
    *,
    provider: str,
    pipeline_config: PipelineConfig,
    query: int,
    stage: str,
    model_config: ModelConfig,
    workload_name: str = "",
    optimization_iter: int = 0,
    budget_tracker: Any | None = None,
) -> AgentBackend:
    registry = build_stage_tool_registry(
        pipeline_config,
        query,
        stage=stage,
        workload_name=workload_name,
        optimization_iter=optimization_iter,
    )
    if budget_tracker is not None:
        from accel_pipeline.optimization_budget import wrap_with_budget

        registry = wrap_with_budget(registry, budget_tracker)
    art_dir = None
    compress_outputs = stage in (
        "implementation",
        "optimization",
        "optimization_analysis",
        "optimization_patch",
        "optimization_unified",
        "optimization_worker",
        "optimization_fix",
        "crash_debug",
        "audit",
    )
    if compress_outputs and workload_name:
        workload = load_workload(workload_name, pipeline_config.repo_root)
        art_dir = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)

    if provider == "cursor":
        return CursorSDKBackend(
            repo_root=pipeline_config.repo_root,
            api_key_env=model_config.api_key_env,
            tool_registry=registry,
            art_dir=art_dir,
            compress_outputs=compress_outputs,
        )

    if provider in {"openai", "openrouter"}:
        return build_openai_agents_backend(
            provider=provider,
            tool_registry=registry,
            stage_tool_names=STAGE_TOOL_NAMES.get(stage, []),
            art_dir=art_dir,
            compress_outputs=compress_outputs,
        )

    raise ValueError(f"unsupported provider: {provider!r}")


__all__ = [
    "AgentBackend",
    "CursorSDKBackend",
    "StageRunRequest",
    "StageRunResult",
    "create_agent_backend",
]
