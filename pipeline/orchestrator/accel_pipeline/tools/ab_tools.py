"""A/B benchmark tools for within-iteration optimization trials."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from accel_pipeline.base_bench import parse_benchmark_log
from accel_pipeline.config import PipelineConfig, resolve_workload_paths
from accel_pipeline.kernel_snapshot import resolve_snapshot_paths
from accel_pipeline.optimization_runtime import record_ab_trial, refresh_optimization_runtime
from accel_pipeline.paths import query_artifacts_dir

CHECKPOINT_DIRNAME = "ab_checkpoint"
ACTIVE_TRIAL_FILE = "ab_active_trial.json"


@dataclass
class AbToolContext:
    pipeline_config: PipelineConfig
    query: int
    workload: dict[str, Any]
    optimization_iter: int = 0


def _art(ctx: AbToolContext) -> Path:
    return query_artifacts_dir(ctx.pipeline_config.repo_root, ctx.query, workload=ctx.workload)


def _checkpoint_dir(ctx: AbToolContext) -> Path:
    return _art(ctx) / CHECKPOINT_DIRNAME


def _active_trial_path(ctx: AbToolContext) -> Path:
    return _art(ctx) / ACTIVE_TRIAL_FILE


def _write_active_trial(ctx: AbToolContext, payload: dict[str, Any]) -> None:
    _active_trial_path(ctx).write_text(json.dumps(payload, indent=2), encoding="utf-8")


def _read_active_trial(ctx: AbToolContext) -> dict[str, Any]:
    path = _active_trial_path(ctx)
    if not path.is_file():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def _clear_active_trial(ctx: AbToolContext) -> None:
    path = _active_trial_path(ctx)
    if path.is_file():
        path.unlink()


def _checkpoint_files(ctx: AbToolContext) -> list[Path]:
    repo = ctx.pipeline_config.repo_root
    return [p for p in resolve_snapshot_paths(ctx.pipeline_config, ctx.workload, ctx.query) if p.is_file()]


def make_ab_tools(ctx: AbToolContext):
    def begin_ab_trial(trial_id: str, hypothesis: str) -> str:
        """Snapshot current kernel files before an A/B trial."""
        if _read_active_trial(ctx):
            return "ERROR: an A/B trial is already active; call end_ab_trial first."
        ckpt = _checkpoint_dir(ctx)
        if ckpt.exists():
            shutil.rmtree(ckpt)
        ckpt.mkdir(parents=True)
        copied: list[str] = []
        for src in _checkpoint_files(ctx):
            rel = src.relative_to(ctx.pipeline_config.repo_root)
            dest = ckpt / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dest)
            copied.append(str(rel))
        if not copied:
            return "ERROR: no kernel files found to checkpoint."
        _write_active_trial(
            ctx,
            {"trial_id": trial_id, "hypothesis": hypothesis, "files": copied},
        )
        return json.dumps(
            {"status": "checkpointed", "trial_id": trial_id, "files": copied},
            indent=2,
        )

    def run_quick_benchmark(include_profile: bool = True) -> str:
        """Run a fast benchmark (+ optional profile row) and return compact metrics."""
        paths = resolve_workload_paths(ctx.pipeline_config, ctx.workload, ctx.query)
        bench_target = paths["bench_target"]
        build_dir = ctx.pipeline_config.build_dir
        repo = ctx.pipeline_config.repo_root
        bin_path = build_dir / bench_target

        build = subprocess.run(
            ["cmake", "--build", str(build_dir), "--target", bench_target, "-j", "4"],
            cwd=repo,
            capture_output=True,
            text=True,
            check=False,
        )
        if build.returncode != 0:
            return f"ERROR: build failed\n{(build.stdout or '') + (build.stderr or '')[-2000:]}"

        env = {
            "ACCEL_BENCH_WARMUP": "1",
            "ACCEL_BENCH_ITERS": "3",
            paths["db_env_var"]: str(paths["dataset_db"]),
        }
        if include_profile:
            env["ACCEL_BENCH_PROFILE"] = "1"

        proc = subprocess.run(
            [str(bin_path)],
            cwd=repo,
            capture_output=True,
            text=True,
            env={**os.environ, **env},
            check=False,
        )
        output = (proc.stdout or "") + (proc.stderr or "")
        if proc.returncode != 0:
            return f"ERROR: benchmark exit={proc.returncode}\n{output[-2000:]}"

        metrics = parse_benchmark_log(output)
        active = _read_active_trial(ctx)
        if active:
            active["last_metrics"] = metrics
            _write_active_trial(ctx, active)

        refresh_optimization_runtime(
            ctx.pipeline_config,
            ctx.workload,
            ctx.query,
            optimization_iter=ctx.optimization_iter,
        )
        return json.dumps(
            {
                "speedup_median": metrics.get("speedup_median"),
                "reference_median_us": metrics.get("reference_median_us"),
                "accelerator_median_us": metrics.get("accelerator_median_us"),
                "pre_scan_us": metrics.get("pre_scan_us"),
                "pre_consume_us": metrics.get("pre_consume_us"),
                "finalize_us": metrics.get("finalize_us"),
                "generic_rows": metrics.get("generic_rows") or metrics.get("generic_fallback_rows"),
                "pre_rows": metrics.get("pre_rows"),
                "dict_chunks": metrics.get("dict_chunks"),
                "flat_chunks": metrics.get("flat_chunks"),
                "distinct_inserts": metrics.get("distinct_inserts"),
            },
            indent=2,
        )

    def end_ab_trial(trial_id: str, keep: bool) -> str:
        """End an A/B trial: keep changes or restore the checkpoint."""
        active = _read_active_trial(ctx)
        if not active:
            return "ERROR: no active A/B trial; call begin_ab_trial first."
        if active.get("trial_id") != trial_id:
            return f"ERROR: active trial is {active.get('trial_id')!r}, not {trial_id!r}."

        metrics = dict(active.get("last_metrics") or {})
        hypothesis = str(active.get("hypothesis") or "")
        ckpt = _checkpoint_dir(ctx)
        restored: list[str] = []

        if not keep:
            for rel in active.get("files") or []:
                src = ckpt / rel
                if not src.is_file():
                    continue
                dest = ctx.pipeline_config.repo_root / rel
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dest)
                restored.append(rel)
            rebuild = subprocess.run(
                [
                    "cmake",
                    "--build",
                    str(ctx.pipeline_config.build_dir),
                    "--target",
                    resolve_workload_paths(ctx.pipeline_config, ctx.workload, ctx.query)["bench_target"],
                    "-j",
                    "4",
                ],
                cwd=ctx.pipeline_config.repo_root,
                capture_output=True,
                text=True,
                check=False,
            )
            if rebuild.returncode != 0:
                return f"ERROR: rebuild after restore failed\n{(rebuild.stdout or '') + (rebuild.stderr or '')[-1500:]}"

        record_ab_trial(
            _art(ctx),
            trial_id=trial_id,
            hypothesis=hypothesis,
            metrics=metrics,
            kept=keep,
        )
        _clear_active_trial(ctx)
        if ckpt.exists():
            shutil.rmtree(ckpt)

        refresh_optimization_runtime(
            ctx.pipeline_config,
            ctx.workload,
            ctx.query,
            optimization_iter=ctx.optimization_iter,
        )
        return json.dumps(
            {
                "trial_id": trial_id,
                "kept": keep,
                "restored_files": restored,
                "metrics": metrics,
            },
            indent=2,
        )

    return begin_ab_trial, run_quick_benchmark, end_ab_trial
