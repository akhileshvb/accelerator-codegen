from __future__ import annotations

from accel_pipeline.config import ModelConfig, PipelineConfig
from accel_pipeline.gates import run_gate
from accel_pipeline.playbook import append_experiment_log, merge_playbook_proposal
from accel_pipeline.stages.base import run_agent_stage


def run(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    max_iterations: int,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
):
    last_result = None
    no_improve_count = 0
    for iteration in range(1, max_iterations + 1):
        # TODO: Track optimization_iter in pipeline_state.json; compact context between iters.
        # TODO: Retention policy from official gate JSON (retain/revert); honor state-sizing SF10 skip.
        # TODO: Stop early when no_improvement_stop consecutive iters show no retained win.
        last_result = run_agent_stage(
            stage="optimization",
            prompt_file="03_optimization.md",
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            query=query,
            user_message=f"Optimization iteration {iteration}: one bounded change only.",
            model_overrides=model_overrides,
            optimization_iter=iteration,
            dry_run=dry_run,
        )
        if dry_run:
            continue

        for gate_name in ("correctness", "baseline", "sf10", "profile"):
            run_gate(pipeline_config, gate_name, query)

        append_experiment_log(
            pipeline_config.repo_root,
            query,
            f"- iter {iteration}: completed gates",
        )
        merge_playbook_proposal(pipeline_config.repo_root, query)
        no_improve_count += 1  # TODO: reset on retained optimization per gate deltas.

    return last_result
