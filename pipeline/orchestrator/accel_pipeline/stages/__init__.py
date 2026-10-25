"""The pipeline's stages by name. A stage's module is imported only when the stage runs, so a run pays for the code of its own stages."""

from __future__ import annotations

import importlib
from types import ModuleType

STAGE_MODULES = {
    "analysis": "accel_pipeline.stages.analysis",
    "test_suite": "accel_pipeline.stages.test_suite",
    "implementation": "accel_pipeline.stages.implementation",
    "optimization": "accel_pipeline.optimization.stage",
    "audit": "accel_pipeline.stages.audit",
}
# what a stage's run() takes besides (pipeline_config, model_config, workload_name, query, model_overrides, dry_run)
STAGE_OPTIONS = {
    "implementation": ("skip_gates",),
    "optimization": ("skip_gates", "resume"),
}


def stage_module(stage: str) -> ModuleType:
    if stage not in STAGE_MODULES:
        raise ValueError(f"unknown stage: {stage}")
    return importlib.import_module(STAGE_MODULES[stage])


def run_stage(stage: str, pipeline_config, model_config, workload_name: str, query: int,
              model_overrides: dict[str, str], dry_run: bool, **options) -> None:
    """Run `stage`, passing along those of `options` that it takes."""
    accepted = {name: options[name] for name in STAGE_OPTIONS.get(stage, ()) if name in options}
    stage_module(stage).run(pipeline_config, model_config, workload_name, query, model_overrides, dry_run, **accepted)
