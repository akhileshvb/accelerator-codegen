from __future__ import annotations

from accel_pipeline.config import ModelConfig, PipelineConfig
from accel_pipeline.gates import run_gate
from accel_pipeline.stages.base import run_agent_stage


def run(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
):
    run_gate(pipeline_config, "scaffold", query)
    # TODO: Auto-apply register_snippet.cpp and cmake_snippet.cmake (idempotent markers in register.cpp / CMakeLists.txt).
    # TODO: Generate tests/test_support/q{N}_manifest.hpp from seed_manifest_root scan.
    result = run_agent_stage(
        stage="implementation",
        prompt_file="02_implementation.md",
        pipeline_config=pipeline_config,
        model_config=model_config,
        workload_name=workload_name,
        query=query,
        user_message="Implement the accelerator, tests, and benchmark. Build and run correctness tests.",
        model_overrides=model_overrides,
        dry_run=dry_run,
    )
    if not dry_run:
        run_gate(pipeline_config, "correctness", query)
        run_gate(pipeline_config, "baseline", query)
    return result
