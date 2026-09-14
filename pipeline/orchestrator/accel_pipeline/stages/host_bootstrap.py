"""Stage: one-shot query-host bootstrap for non-duckdb engines."""

from __future__ import annotations

from accel_pipeline.config import ModelConfig, PipelineConfig
from accel_pipeline.stages.base import run_agent_stage


def run(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
    *,
    engine_name: str = "",
    extra_user_message: str = "",
):
    engine = engine_name or "engine"
    user_message = (
        f"Bootstrap the query host for engine={engine}: SQL entrypoints, "
        f"differential harness test_{engine}_tpch_differential, and shared "
        "registration helpers. Replicate duckdb gate surfaces for this backend. "
        "Do not invent a second adapter tree. Postgres: Docker ninja only "
        "(request_gate host_bootstrap) — do not cmake/make on the developer "
        "host. Call request_gate host_bootstrap until it passes, then finish."
    )
    extra = (extra_user_message or "").strip()
    if extra:
        user_message = f"{user_message}\n\n{extra}"
    return run_agent_stage(
        stage="host_bootstrap",
        prompt_file="host_bootstrap.md",
        pipeline_config=pipeline_config,
        model_config=model_config,
        workload_name=workload_name,
        query=query,
        user_message=user_message,
        model_overrides=model_overrides,
        dry_run=dry_run,
    )
