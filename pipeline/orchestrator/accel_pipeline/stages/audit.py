from __future__ import annotations

from accel_pipeline.config import ModelConfig, PipelineConfig, load_run_workload
from accel_pipeline.repair import RepairCtx, run_with_repair
from accel_pipeline.session import READONLY
from accel_pipeline.stages.gate_plan import workload_post_audit_gates
from accel_pipeline.stages.base import run_agent_stage


def run(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
):
    def audit():
        return run_agent_stage(
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
            session_role=READONLY,
        )

    if dry_run:
        return audit()
    # a failing audit gate goes to the repair loop (the fix runs in the implementation session, not the reviewer's)
    ctx = RepairCtx(pipeline_config, model_config, workload_name, query, stage="audit",
                    change_kind="audit", model_overrides=model_overrides)
    return run_with_repair(ctx, audit, gates=workload_post_audit_gates(load_run_workload(pipeline_config, workload_name)))
