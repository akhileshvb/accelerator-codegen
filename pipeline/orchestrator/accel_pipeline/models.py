from __future__ import annotations

import os

from accel_pipeline.config import ModelConfig, PipelineConfig


def resolve_preset(stage: str, model_config: ModelConfig, overrides: dict[str, str] | None = None) -> str:
    overrides = overrides or {}
    preset_name = overrides.get(stage) or model_config.stages.get(stage, "default")
    if preset_name not in model_config.presets:
        raise KeyError(f"unknown model preset: {preset_name}")
    return model_config.presets[preset_name]


def _require_api_key(model_config: ModelConfig) -> None:
    if model_config.api_key_env not in os.environ and os.environ.get("ACCEL_PIPELINE_DRY_RUN") != "1":
        raise EnvironmentError(f"set {model_config.api_key_env} or use --dry-run")


def make_model_id(
    stage: str,
    pipeline_config: PipelineConfig,  # noqa: ARG001 — reserved for future provider options
    model_config: ModelConfig,
    overrides: dict[str, str] | None = None,
) -> str:
    """Return the model id string for the configured provider."""
    provider = model_config.provider
    model_id = resolve_preset(stage, model_config, overrides)

    if provider == "cursor":
        _require_api_key(model_config)
        return model_id

    if provider == "openai":
        _require_api_key(model_config)
        return model_id

    if provider == "openrouter":
        _require_api_key(model_config)
        # TODO(openrouter): return LitellmModel(openrouter/{model_id}) for OpenAI Agents backend.
        raise NotImplementedError(
            "provider 'openrouter' is not implemented yet; "
            "use provider: cursor or provider: openai, or add LitellmModel support."
        )

    raise ValueError(f"unsupported provider: {provider!r}")
