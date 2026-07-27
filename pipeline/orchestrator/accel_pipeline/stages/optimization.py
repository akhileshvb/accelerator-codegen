from __future__ import annotations

from accel_pipeline.aux_bench import load_optimization_state, process_aux_bench_after_gates
from accel_pipeline.config import ModelConfig, PipelineConfig, load_workload
from accel_pipeline.gates import run_gate
from accel_pipeline.stages.gate_plan import workload_post_optimization_gates
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
    workload = load_workload(workload_name, pipeline_config.repo_root)
    opt_gates = workload_post_optimization_gates(workload)
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

        prev_best = float(load_optimization_state(pipeline_config.repo_root, query).get("best_composite_score", 0.0) or 0.0)
        for gate_name in opt_gates:
            run_gate(pipeline_config, gate_name, query, workload=workload)

        if "aux_bench_sweep" in opt_gates:
            process_aux_bench_after_gates(
                pipeline_config.repo_root,
                query,
                iteration=iteration,
                change_kind="optimization",
            )
            new_best = float(
                load_optimization_state(pipeline_config.repo_root, query).get("best_composite_score", 0.0) or 0.0
            )
            if new_best > prev_best:
                no_improve_count = 0
            else:
                no_improve_count += 1
        else:
            append_experiment_log(
                pipeline_config.repo_root,
                query,
                f"- iter {iteration}: completed gates",
            )
            no_improve_count += 1

        merge_playbook_proposal(pipeline_config.repo_root, query)
        if no_improve_count >= 2:
            append_experiment_log(
                pipeline_config.repo_root,
                query,
                f"- iter {iteration}: stopping early after {no_improve_count} iterations without bench improvement",
            )
            break

    return last_result
