from __future__ import annotations

import os

from accel_pipeline.config import ModelConfig, PipelineConfig


def resolve_preset(stage: str, model_config: ModelConfig, overrides: dict[str, str] | None = None) -> str:
    overrides = overrides or {}
    preset_name = overrides.get(stage) or model_config.stages.get(stage, "default")
    if preset_name not in model_config.presets:
        raise KeyError(f"unknown model preset: {preset_name}")
    return model_config.presets[preset_name]


def make_model_id(
    stage: str,
    pipeline_config: PipelineConfig,  # noqa: ARG001 — reserved for future provider options
    model_config: ModelConfig,
    overrides: dict[str, str] | None = None,
) -> str:
    """Return model id string for Agent construction.

    MVP: OpenAI provider — pass model name, SDK uses OPENAI_API_KEY.
    TODO(openrouter): return LitellmModel(...) when provider == openrouter.
    """
    if model_config.provider != "openai":
        raise NotImplementedError(
            f"provider {model_config.provider!r} not implemented; "
            "TODO: add OpenRouter via LitellmModel"
        )
    if model_config.api_key_env not in os.environ and os.environ.get("ACCEL_PIPELINE_DRY_RUN") != "1":
        raise EnvironmentError(
            f"set {model_config.api_key_env} or use --dry-run"
        )
    return resolve_preset(stage, model_config, overrides)
