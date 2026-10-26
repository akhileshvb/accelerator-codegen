from __future__ import annotations

import json
import sys
import time
from functools import partial
from pathlib import Path
from typing import Any

from accel_pipeline.aux_bench import load_optimization_state, process_aux_bench_after_gates
from accel_pipeline.base_bench import load_base_optimization_state, process_base_bench_after_gates
from accel_pipeline.config import ModelConfig, PipelineConfig, load_run_workload
from accel_pipeline.crash_debug import run_post_stage_gates_with_crash_debug
from accel_pipeline.kernel_snapshot import (
    has_best_kernel_snapshot,
    maybe_restore_best_kernel,
    rebuild_workload_targets,
    restore_best_kernel,
)
from accel_pipeline.optimization_attempt import run_optimization_iteration
from accel_pipeline.optimization_checkpoint import load_best_valid_baseline
from accel_pipeline.optimization_worker import OptimizationWorker
from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.playbook import append_experiment_log, merge_playbook_proposal
from accel_pipeline.repair import RepairCtx, run_with_repair
from accel_pipeline.stages.gate_plan import workload_post_optimization_gates


def _aux_snapshot_is_ahead(art: Path, iteration: int) -> bool:
    """Whether to start the iteration from the fair-sweep best kernel: always after the first iteration, and on the
    first when the quick best_valid has drifted below that snapshot (noisy quick accepts)."""
    if not has_best_kernel_snapshot(art):
        return False
    if iteration > 1:
        return True
    try:
        manifest = json.loads((art / "best_kernel_snapshot" / "manifest.json").read_text(encoding="utf-8"))
        fair_best = float(manifest.get("speedup_median") or 0.0)
    except (json.JSONDecodeError, OSError, TypeError, ValueError):
        fair_best = 0.0
    quick_best = float(load_best_valid_baseline(art).get("score") or 0.0)
    return fair_best > 0 and (quick_best <= 0 or quick_best + 0.05 < fair_best)


def _restore_best_kernel(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, art: Path,
                         iteration: int) -> None:
    if workload.get("workload_kind") != "aux_state":
        state = load_base_optimization_state(art)
        maybe_restore_best_kernel(
            pipeline_config,
            workload,
            query,
            best_speedup_median=float(state.get("best_speedup_median") or 0.0),
            last_speedup_median=float(state.get("last_speedup_median") or 0.0),
            reason=f"optimization iter {iteration} start",
        )
        return
    if not _aux_snapshot_is_ahead(art, iteration):
        return
    manifest = restore_best_kernel(pipeline_config, workload, query)
    if manifest is None:
        return
    rebuild_workload_targets(pipeline_config, workload, query)
    append_experiment_log(
        pipeline_config.repo_root,
        query,
        f"- optimization iter {iteration} start: restored best aux kernel "
        f"({manifest.get('speedup_median', 0):.2f}x read path from {manifest.get('change_kind', 'unknown')} iter "
        f"{manifest.get('iteration', '?')})",
        workload=workload,
    )
    # The next baseline measures the restored tree.
    (art / "best_valid_baseline.json").unlink(missing_ok=True)


def _best_score(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, art: Path) -> float:
    if workload.get("workload_kind") == "aux_state":
        state = load_optimization_state(pipeline_config.repo_root, query, workload)
        return float(state.get("best_composite_score") or 0.0)
    return float(load_base_optimization_state(art).get("best_speedup_median") or 0.0)


def _record_iteration_bench(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, art: Path,
                            iteration: int) -> bool:
    """Fold the end-of-iteration fair sweep into the optimization state; whether it improved the best."""
    if workload.get("workload_kind") != "aux_state":
        bench = process_base_bench_after_gates(pipeline_config.repo_root, query, workload, iteration=iteration,
                                               change_kind="optimization")
        return bool(bench.get("improved"))
    previous = _best_score(pipeline_config, workload, query, art)
    process_aux_bench_after_gates(pipeline_config.repo_root, query, iteration=iteration, change_kind="optimization",
                                  workload=workload)
    return _best_score(pipeline_config, workload, query, art) > previous


def run(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    max_iterations: int,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
    *,
    skip_gates: bool = False,
    resume: bool = False,
):
    if dry_run:
        return {"dry_run": True, "stage": "optimization",
                "max_attempts": pipeline_config.optimization_max_attempts_per_iteration}
    workload = load_run_workload(pipeline_config, workload_name)
    opt_gates = workload_post_optimization_gates(workload)
    budget = pipeline_config.optimization_stage_budget_sec
    deadline = time.time() + budget if budget > 0 else None
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)

    def log(message: str) -> None:
        append_experiment_log(pipeline_config.repo_root, query, message, workload=workload)

    last_result = None
    no_improve_count = 0
    worker = OptimizationWorker.create(
        pipeline_config=pipeline_config,
        model_config=model_config,
        workload_name=workload_name,
        query=query,
        model_overrides=model_overrides,
        resume=resume,
    )
    try:
        for iteration in range(1, max_iterations + 1):
            if deadline is not None and time.time() >= deadline:
                log(f"- iter {iteration}: optimization stage budget ({budget:.0f}s) spent; going to audit")
                break
            _restore_best_kernel(pipeline_config, workload, query, art, iteration)

            iterate = partial(
                run_optimization_iteration,
                pipeline_config=pipeline_config,
                model_config=model_config,
                workload_name=workload_name,
                query=query,
                optimization_iter=iteration,
                worker=worker,
                model_overrides=model_overrides,
                deadline=deadline,
            )
            repair_ctx = RepairCtx(pipeline_config, model_config, workload_name, query, stage="optimization",
                                   change_kind="optimization", iteration=iteration, model_overrides=model_overrides)
            last_result = run_with_repair(repair_ctx, iterate)
            saturated = last_result is not None and last_result.status == "saturated"
            if skip_gates:
                if saturated:
                    log(f"- iter {iteration}: agent declared OPTIMIZATION_SATURATED; skipping remaining iters")
                    break
                continue

            print(f"[pipeline] optimization iter {iteration}: running full gate batch "
                  f"({len(opt_gates)} gates, end-of-iteration only)", file=sys.stderr, flush=True)
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
            improved = _record_iteration_bench(pipeline_config, workload, query, art, iteration)
            no_improve_count = 0 if improved else no_improve_count + 1
            merge_playbook_proposal(pipeline_config.repo_root, query, workload=workload)

            if saturated:
                log(f"- iter {iteration}: OPTIMIZATION_SATURATED — ran end-of-iter gates, skipping remaining iterations")
                break
            if no_improve_count >= pipeline_config.optimization_no_improvement_stop:
                log(f"- iter {iteration}: stopping after {no_improve_count} iterations without improvement")
                break
    finally:
        worker.close()

    return last_result
