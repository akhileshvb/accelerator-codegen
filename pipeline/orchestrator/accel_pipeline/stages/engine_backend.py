"""Engine backend onboarding stages (substrate-first)."""

from __future__ import annotations

from pathlib import Path

from accel_pipeline.config import ModelConfig, PipelineConfig, load_workload, apply_workload_overrides, resolve_workload_paths
from accel_pipeline.crash_debug import run_post_stage_gates_with_crash_debug
from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.stages.base import run_agent_stage
from accel_pipeline.stages.gate_plan import run_post_stage_gates


ENGINE_STAGES = ("analysis", "implementation", "validation")


def _engine_exists(pipeline_config: PipelineConfig, workload: dict) -> bool:
    targets = workload.get("target_paths") or {}
    header = targets.get("scan_adapter_header")
    if not header:
        return False
    return (pipeline_config.repo_root / header).is_file()


def _user_message(pipeline_config: PipelineConfig, workload: dict, stage: str) -> str:
    engine = workload.get("engine_name", "engine")
    exists = _engine_exists(pipeline_config, workload)
    mode = "update_existing" if exists and workload.get("allow_update_existing", True) else "generate_from_scratch"
    return (
        f"Engine backend stage={stage} engine={engine} mode={mode}. "
        "Primary context is the fixed substrate (core/state/maintenance + NativeScanSpec). "
        "Optional example backends in the prompt are few-shot only — do not require them on disk."
    )


def run_analysis(pipeline_config, model_config, workload_name, query, model_overrides, dry_run):
    workload = load_workload(workload_name, pipeline_config.repo_root)
    return run_agent_stage(
        stage="analysis",
        prompt_file="01_analysis.md",
        pipeline_config=pipeline_config,
        model_config=model_config,
        workload_name=workload_name,
        query=query,
        user_message=_user_message(pipeline_config, workload, "analysis"),
        model_overrides=model_overrides,
        dry_run=dry_run,
    )


def run_implementation(
    pipeline_config, model_config, workload_name, query, model_overrides, dry_run, *, skip_gates=False
):
    workload = load_workload(workload_name, pipeline_config.repo_root)
    result = run_agent_stage(
        stage="implementation",
        prompt_file="02_implementation.md",
        pipeline_config=pipeline_config,
        model_config=model_config,
        workload_name=workload_name,
        query=query,
        user_message=_user_message(pipeline_config, workload, "implementation"),
        model_overrides=model_overrides,
        dry_run=dry_run,
    )
    if dry_run or skip_gates:
        return result
    gates = list(workload.get("post_implementation_gates") or ["engine_build"])
    run_post_stage_gates(pipeline_config, workload_name, query, gates)
    return result


def run_validation(
    pipeline_config, model_config, workload_name, query, model_overrides, dry_run, *, skip_gates=False
):
    workload = load_workload(workload_name, pipeline_config.repo_root)
    result = run_agent_stage(
        stage="validation",
        prompt_file="03_validation.md",
        pipeline_config=pipeline_config,
        model_config=model_config,
        workload_name=workload_name,
        query=query,
        user_message=_user_message(pipeline_config, workload, "validation"),
        model_overrides=model_overrides,
        dry_run=dry_run,
    )
    if dry_run or skip_gates:
        return result
    gates = list(workload.get("post_validation_gates") or ["engine_build", "engine_scan_conformance"])
    run_post_stage_gates(pipeline_config, workload_name, query, gates)
    return result


def run_engine_pipeline(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str],
    dry_run: bool,
    *,
    stage: str = "all",
    skip_gates: bool = False,
) -> None:
    if stage == "analysis":
        run_analysis(pipeline_config, model_config, workload_name, query, model_overrides, dry_run)
        return
    if stage == "implementation":
        run_implementation(
            pipeline_config, model_config, workload_name, query, model_overrides, dry_run, skip_gates=skip_gates
        )
        return
    if stage in ("validation", "audit"):
        run_validation(
            pipeline_config, model_config, workload_name, query, model_overrides, dry_run, skip_gates=skip_gates
        )
        return

    run_analysis(pipeline_config, model_config, workload_name, query, model_overrides, dry_run)
    run_implementation(
        pipeline_config, model_config, workload_name, query, model_overrides, dry_run, skip_gates=skip_gates
    )
    run_validation(
        pipeline_config, model_config, workload_name, query, model_overrides, dry_run, skip_gates=skip_gates
    )
