from __future__ import annotations

import sys
import time

from accel_pipeline.optimization.aux_bench import load_optimization_state, process_aux_bench_after_gates
from accel_pipeline.optimization.kernel_snapshot import maybe_restore_best_kernel
from accel_pipeline.optimization.base_bench import (
    load_base_optimization_state,
    process_base_bench_after_gates,
)
from accel_pipeline.config import ModelConfig, PipelineConfig, load_run_workload, query_artifacts_dir
from accel_pipeline.repair import run_gates_with_repair, RepairCtx, run_with_repair
from accel_pipeline.optimization.attempt import run_optimization_iteration
from accel_pipeline.optimization.worker import OptimizationWorker, worker_enabled
from accel_pipeline.stages import workload_post_optimization_gates
from accel_pipeline.optimization.runtime import merge_playbook_proposal
from accel_pipeline.runlog import append_experiment_log


def run(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
    *,
    skip_gates: bool = False,
    resume: bool = False,
):
    last_result = None
    no_improve_count = 0
    workload = load_run_workload(pipeline_config, workload_name)
    opt_gates = workload_post_optimization_gates(workload)
    no_improve_stop = pipeline_config.optimization_no_improvement_stop
    budget = float(getattr(pipeline_config, "optimization_stage_budget_sec", 0) or 0)
    deadline = time.time() + budget if budget > 0 else None
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)

    worker: OptimizationWorker | None = None
    if not dry_run and worker_enabled(pipeline_config):
        worker = OptimizationWorker.create(
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            query=query,
            model_overrides=model_overrides,
            resume=resume,
        )

    try:
        for iteration in range(1, pipeline_config.optimization_max_iterations + 1):
            if deadline is not None and time.time() >= deadline and not dry_run:
                append_experiment_log(
                    pipeline_config.repo_root,
                    query,
                    f"- iter {iteration}: optimization stage budget ({budget:.0f}s) spent; going to audit",
                    workload=workload,
                )
                break
            restored = False
            if not dry_run:
                if workload.get("workload_kind") != "aux_state":
                    state = load_base_optimization_state(art)
                    restored = maybe_restore_best_kernel(
                        pipeline_config,
                        workload,
                        query,
                        best_speedup_median=float(state.get("best_speedup_median") or 0.0),
                        last_speedup_median=float(state.get("last_speedup_median") or 0.0),
                        reason=f"optimization iter {iteration} start",
                    )
                else:
                    from accel_pipeline.optimization.kernel_snapshot import (
                        has_best_kernel_snapshot,
                        rebuild_workload_targets,
                        restore_best_kernel,
                    )
                    from accel_pipeline.optimization.checkpoint import load_best_valid_baseline

                    # Always start from the fair-sweep best_kernel when the working tree /
                    # best_valid has drifted below that snapshot (noisy quick accepts).
                    should_restore = False
                    if has_best_kernel_snapshot(art):
                        if iteration > 1:
                            should_restore = True
                        else:
                            manifest_path = art / "best_kernel_snapshot" / "manifest.json"
                            try:
                                import json

                                manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
                                fair_best = float(manifest.get("speedup_median") or 0.0)
                            except (json.JSONDecodeError, OSError, TypeError, ValueError):
                                fair_best = 0.0
                            best_valid = load_best_valid_baseline(art)
                            quick_best = float(best_valid.get("score") or 0.0)
                            if fair_best > 0 and (quick_best <= 0 or quick_best + 0.05 < fair_best):
                                should_restore = True
                    if should_restore and has_best_kernel_snapshot(art):
                        manifest = restore_best_kernel(pipeline_config, workload, query)
                        if manifest is not None:
                            rebuild_workload_targets(pipeline_config, workload, query)
                            append_experiment_log(
                                pipeline_config.repo_root,
                                query,
                                f"- optimization iter {iteration} start: restored best aux kernel "
                                f"({manifest.get('speedup_median', 0):.2f}x read path from "
                                f"{manifest.get('change_kind', 'unknown')} iter "
                                f"{manifest.get('iteration', '?')})",
                                workload=workload,
                            )
                            restored = True
                            # Reset best_valid so the next measure re-seeds from restored tree.
                            best_valid_path = art / "best_valid_baseline.json"
                            if best_valid_path.is_file():
                                best_valid_path.unlink()

            def iterate():
                return run_optimization_iteration(
                    pipeline_config=pipeline_config,
                    model_config=model_config,
                    workload_name=workload_name,
                    query=query,
                    optimization_iter=iteration,
                    model_overrides=model_overrides,
                    dry_run=dry_run,
                    restored=restored,
                    worker=worker,
                    deadline=deadline,
                )

            if dry_run:
                last_result = iterate()
                continue
            repair_ctx = RepairCtx(pipeline_config, model_config, workload_name, query, stage="optimization",
                                   change_kind="optimization", iteration=iteration, model_overrides=model_overrides)
            last_result = run_with_repair(repair_ctx, iterate)
            if dry_run:
                continue

            saturated = getattr(last_result, "status", None) == "saturated"
            if saturated and skip_gates:
                append_experiment_log(
                    pipeline_config.repo_root,
                    query,
                    f"- iter {iteration}: agent declared OPTIMIZATION_SATURATED; skipping remaining iters",
                    workload=workload,
                )
                break

            if skip_gates:
                continue

            print(
                f"[pipeline] optimization iter {iteration}: running full gate batch "
                f"({len(opt_gates)} gates, end-of-iteration only)",
                file=sys.stderr,
                flush=True,
            )
            run_gates_with_repair(
                pipeline_config,
                model_config,
                workload_name,
                query,
                opt_gates,
                change_kind="optimization",
                iteration=iteration,
                model_overrides=model_overrides,
            )

            if workload.get("workload_kind") == "aux_state":
                prev_best = float(
                    load_optimization_state(pipeline_config.repo_root, query, workload).get(
                        "best_composite_score", 0.0
                    )
                    or 0.0
                )
            else:
                prev_best = float(load_base_optimization_state(art).get("best_speedup_median", 0.0) or 0.0)

            if workload.get("workload_kind") == "aux_state":
                process_aux_bench_after_gates(
                    pipeline_config.repo_root,
                    query,
                    iteration=iteration,
                    change_kind="optimization",
                    workload=workload,
                )
                new_best = float(
                    load_optimization_state(pipeline_config.repo_root, query, workload).get(
                        "best_composite_score", 0.0
                    )
                    or 0.0
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

            if saturated:
                append_experiment_log(
                    pipeline_config.repo_root,
                    query,
                    f"- iter {iteration}: OPTIMIZATION_SATURATED — ran end-of-iter gates, "
                    "skipping remaining iterations",
                    workload=workload,
                )
                break

            if no_improve_count >= no_improve_stop:
                append_experiment_log(
                    pipeline_config.repo_root,
                    query,
                    f"- iter {iteration}: stopping after {no_improve_count} iterations without improvement",
                    workload=workload,
                )
                break

    finally:
        if worker is not None:
            worker.close()

    return last_result
