from __future__ import annotations

import os

from accel_pipeline.config import ModelConfig, PipelineConfig


_OPTIMIZATION_STAGE_FALLBACKS = (
    "optimization",
    "optimization_unified",
    "optimization_unified_hypothesis",
    "optimization_unified_patch",
    "optimization_fix",
    "optimization_worker",
)

_STAGE_PROVIDER_PARENTS = {
    "optimization": "optimization_unified",
    "optimization_unified_hypothesis": "optimization_unified",
    "optimization_unified_patch": "optimization_unified",
    "optimization_fix": "optimization_worker",
    "optimization_worker": "optimization_unified",
    "preflight_live": "analysis",  # the live model probe runs the way an analysis turn does
}


def is_implementation_stage(stage: str) -> bool:
    return stage == "implementation" or stage.startswith("impl_")


def is_critique_stage(stage: str) -> bool:
    return stage == "layer_critique" or stage.startswith("critique_")


def _implementation_family(stage: str) -> bool:
    return is_implementation_stage(stage)


def resolve_stage_provider(stage: str, model_config: ModelConfig) -> str:
    """Per-stage provider: the stage's own entry, else its family or parent stage's; unknown stages raise."""
    providers = model_config.stage_providers
    if stage in providers:
        return providers[stage]
    if is_critique_stage(stage) and "layer_critique" in providers:
        return providers["layer_critique"]
    if _implementation_family(stage) and "implementation" in providers:
        return providers["implementation"]
    parent = _STAGE_PROVIDER_PARENTS.get(stage)
    if parent and parent in providers:
        return providers[parent]
    if stage in _OPTIMIZATION_STAGE_FALLBACKS and "optimization_unified" in providers:
        return providers["optimization_unified"]
    raise KeyError(f"no provider configured for stage {stage!r} (stage_providers)")


def resolve_preset(stage: str, model_config: ModelConfig, overrides: dict[str, str] | None = None) -> str:
    overrides = overrides or {}
    preset_name = overrides.get(stage)
    if not preset_name and _implementation_family(stage):
        preset_name = overrides.get("implementation")
    if not preset_name and is_critique_stage(stage):
        preset_name = overrides.get("layer_critique")
    if not preset_name:
        preset_name = model_config.stages.get(stage)
    if not preset_name and is_critique_stage(stage):
        preset_name = model_config.stages.get("layer_critique")
        if not preset_name and "cheap_code" in model_config.presets:
            preset_name = "cheap_code"
    if not preset_name and _implementation_family(stage):
        preset_name = model_config.stages.get("implementation")
    if not preset_name and stage in _OPTIMIZATION_STAGE_FALLBACKS:
        preset_name = (
            model_config.stages.get("optimization_unified")
            or model_config.stages.get("optimization_worker")
            or "default"
        )
    preset_name = preset_name or "default"
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
    """Return the model id string for logging and dry-run display."""
    provider = resolve_stage_provider(stage, model_config)
    if provider != "codex_exec":
        raise ValueError(f"unsupported provider: {provider!r}")
    _require_api_key(model_config)
    return resolve_preset(stage, model_config, overrides)
