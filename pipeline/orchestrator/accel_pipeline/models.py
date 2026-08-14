from __future__ import annotations

import os
from typing import Any

from accel_pipeline.backends.openai_agents import build_openrouter_model
from accel_pipeline.config import ModelConfig, PipelineConfig


def resolve_preset(stage: str, model_config: ModelConfig, overrides: dict[str, str] | None = None) -> str:
    overrides = overrides or {}
    preset_name = overrides.get(stage) or model_config.stages.get(stage)
    if not preset_name and stage in _OPTIMIZATION_STAGE_FALLBACKS:
        preset_name = model_config.stages.get("optimization", "default")
    preset_name = preset_name or "default"
    if preset_name not in model_config.presets:
        raise KeyError(f"unknown model preset: {preset_name}")
    return model_config.presets[preset_name]


_OPTIMIZATION_STAGE_FALLBACKS = (
    "optimization_analysis",
    "optimization_patch",
    "optimization_unified",
    "optimization_unified_hypothesis",
    "optimization_unified_patch",
    "optimization_fix",
    "optimization_worker",
)


def _require_api_key(model_config: ModelConfig) -> None:
    if model_config.api_key_env not in os.environ and os.environ.get("ACCEL_PIPELINE_DRY_RUN") != "1":
        raise EnvironmentError(f"set {model_config.api_key_env} or use --dry-run")


def make_model_id(
    stage: str,
    pipeline_config: PipelineConfig,  # noqa: ARG001 — reserved for future provider options
    model_config: ModelConfig,
    overrides: dict[str, str] | None = None,
) -> str:
    """Return the model id string for logging and dry-run display."""
    provider = model_config.provider
    model_id = resolve_preset(stage, model_config, overrides)

    if provider in {"cursor", "openai", "openrouter"}:
        if provider != "cursor" or model_config.api_key_env:
            _require_api_key(model_config)
        return model_id

    raise ValueError(f"unsupported provider: {provider!r}")


def resolve_agent_model(
    stage: str,
    pipeline_config: PipelineConfig,  # noqa: ARG001 — reserved for future provider options
    model_config: ModelConfig,
    overrides: dict[str, str] | None = None,
) -> str | Any:
    """Return the model object/string passed to the agent backend."""
    provider = model_config.provider
    preset = resolve_preset(stage, model_config, overrides)

    if provider == "cursor":
        _require_api_key(model_config)
        return preset

    if provider == "openai":
        _require_api_key(model_config)
        return preset

    if provider == "openrouter":
        return build_openrouter_model(
            preset,
            api_key_env=model_config.api_key_env,
            base_url=model_config.openrouter_base_url,
        )

    raise ValueError(f"unsupported provider: {provider!r}")
