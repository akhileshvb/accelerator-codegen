from __future__ import annotations

from accel_pipeline.aux_bench import load_optimization_state, process_aux_bench_after_gates
from accel_pipeline.kernel_snapshot import maybe_restore_best_kernel
from accel_pipeline.base_bench import (
    load_base_optimization_state,
    process_base_bench_after_gates,
)
from accel_pipeline.config import ModelConfig, PipelineConfig, load_workload
from accel_pipeline.crash_debug import run_post_stage_gates_with_crash_debug
from accel_pipeline.optimization_episodic import run_optimization_iteration
from accel_pipeline.optimization_runtime import save_ab_trials
from accel_pipeline.paths import query_artifacts_dir
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
    is_aux = workload.get("workload_kind") == "aux_state"
    opt_gates = workload_post_optimization_gates(workload)
    no_improve_stop = pipeline_config.optimization_no_improvement_stop
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)

    for iteration in range(1, max_iterations + 1):
        restored = False
        if not dry_run and not is_aux:
            state = load_base_optimization_state(art)
            restored = maybe_restore_best_kernel(
                pipeline_config,
                workload,
                query,
                best_speedup_median=float(state.get("best_speedup_median") or 0.0),
                last_speedup_median=float(state.get("last_speedup_median") or 0.0),
                reason=f"optimization iter {iteration} start",
            )
            save_ab_trials(art, [])

        if is_aux:
            last_result = run_agent_stage(
                stage="optimization",
                prompt_file="03_optimization.md",
                pipeline_config=pipeline_config,
                model_config=model_config,
                workload_name=workload_name,
                query=query,
                user_message=(
                    f"Optimization iteration {iteration}/{max_iterations}: apply an aggressive bundle "
                    "of coordinated optimizations."
                ),
                model_overrides=model_overrides,
                optimization_iter=iteration,
                dry_run=dry_run,
            )
        else:
            last_result = run_optimization_iteration(
                pipeline_config=pipeline_config,
                model_config=model_config,
                workload_name=workload_name,
                query=query,
                optimization_iter=iteration,
                model_overrides=model_overrides,
                dry_run=dry_run,
                restored=restored,
            )
        if dry_run:
            continue

        if is_aux:
            from accel_pipeline.stages.gate_plan import run_post_stage_gates

            run_post_stage_gates(
                pipeline_config,
                workload_name,
                query,
                opt_gates,
                change_kind="optimization",
                iteration=iteration,
            )
        else:
            run_post_stage_gates_with_crash_debug(
                pipeline_config,
                model_config,
                workload_name,
                query,
                opt_gates,
                change_kind="optimization",
                iteration=iteration,
                model_overrides=model_overrides,
            )

        if is_aux:
            prev_best = float(
                load_optimization_state(pipeline_config.repo_root, query).get("best_composite_score", 0.0) or 0.0
            )
        else:
            prev_best = float(load_base_optimization_state(art).get("best_speedup_median", 0.0) or 0.0)

        if is_aux:
            process_aux_bench_after_gates(
                pipeline_config.repo_root,
                query,
                iteration=iteration,
                change_kind="optimization",
            )
            new_best = float(
                load_optimization_state(pipeline_config.repo_root, query).get("best_composite_score", 0.0) or 0.0
            )
            improved = new_best > prev_best
        else:
            bench = process_base_bench_after_gates(
                pipeline_config.repo_root,
                query,
                workload,
                iteration=iteration,
                change_kind="optimization",
            )
            improved = bool(bench.get("improved"))
            new_best = float(bench.get("speedup_median") or 0.0)

        if improved:
            no_improve_count = 0
        else:
            no_improve_count += 1

        merge_playbook_proposal(pipeline_config.repo_root, query, workload=workload)

        if no_improve_count >= no_improve_stop:
            append_experiment_log(
                pipeline_config.repo_root,
                query,
                f"- iter {iteration}: stopping after {no_improve_count} iterations without improvement",
                workload=workload,
            )
            break

    return last_result
