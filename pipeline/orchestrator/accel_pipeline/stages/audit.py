from __future__ import annotations

from accel_pipeline.config import ModelConfig, PipelineConfig, load_run_workload
from accel_pipeline.crash_debug import run_post_stage_gates_with_crash_debug
from accel_pipeline.stages.gate_plan import workload_post_audit_gates
from accel_pipeline.stages.base import run_agent_stage

AUDIT_FIX_ROUNDS = 3


def run(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
):
    result = run_agent_stage(
        stage="audit",
        prompt_file="04_audit.md",
        pipeline_config=pipeline_config,
        model_config=model_config,
        workload_name=workload_name,
        query=query,
        user_message=(
            "Produce final_audit.md: verify correctness, cite SF1 sweep numbers, "
            "and review scale-safety + harness fairness (no brittle gate chasing)."
        ),
        model_overrides=model_overrides,
        dry_run=dry_run,
    )
    if not dry_run:
        # a failing audit gate goes back to a bounded crash-debug fix (the failure details, then the gates again)
        # instead of ending the run after all the work before it
        run_post_stage_gates_with_crash_debug(
            pipeline_config,
            model_config,
            workload_name,
            query,
            workload_post_audit_gates(load_run_workload(pipeline_config, workload_name)),
            change_kind="audit",
            model_overrides=model_overrides,
            max_rounds=AUDIT_FIX_ROUNDS,
        )
    return result
