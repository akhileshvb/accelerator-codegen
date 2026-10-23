from __future__ import annotations

from typing import Any

from accel_pipeline.backends.base import AgentBackend, StageRunRequest, StageRunResult
from accel_pipeline.config import ModelConfig, PipelineConfig, load_run_workload
from accel_pipeline.paths import query_artifacts_dir

_ARTIFACT_STAGES = frozenset(
    {
        "implementation",
        "optimization_unified",
        "optimization_unified_hypothesis",
        "optimization_unified_patch",
        "optimization_worker",
        "optimization_fix",
        "crash_debug",
        "audit",
    }
)


def create_agent_backend(
    *,
    provider: str | None = None,
    pipeline_config: PipelineConfig,
    query: int,
    stage: str,
    model_config: ModelConfig,
    workload_name: str = "",
    optimization_iter: int = 0,
) -> AgentBackend:
    from accel_pipeline.models import resolve_stage_provider

    resolved = provider or resolve_stage_provider(stage, model_config)
    if resolved != "codex_exec":
        raise ValueError(f"unsupported provider: {resolved!r}")

    art_dir = None
    if stage in _ARTIFACT_STAGES and workload_name:
        workload = load_run_workload(pipeline_config, workload_name)
        art_dir = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)

    from accel_pipeline.backends.codex_exec import CodexExecBackend

    backend = CodexExecBackend(
        pipeline_config=pipeline_config,
        model_config=model_config,
        query=query,
        stage=stage,
        workload_name=workload_name,
        optimization_iter=optimization_iter,
        art_dir=art_dir,
    )
    if getattr(pipeline_config, "sandbox", "none") == "bwrap":
        from accel_pipeline import sandbox

        backend.command_prefix = sandbox.cpp_prefix(pipeline_config)
        backend.prompt_via_stdin = True  # the sandbox has its own /tmp: no task file to read back
    return backend


__all__ = [
    "AgentBackend",
    "StageRunRequest",
    "StageRunResult",
    "create_agent_backend",
]
