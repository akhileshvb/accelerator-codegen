"""A/B benchmark tools for within-iteration optimization trials."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from accel_pipeline.aux_bench import parse_aux_benchmark_output, summarize_sweep_rows
from accel_pipeline.base_bench import parse_benchmark_log
from accel_pipeline.config import PipelineConfig, resolve_workload_paths
from accel_pipeline.kernel_snapshot import resolve_snapshot_paths
from accel_pipeline.optimization_runtime import record_ab_trial, refresh_optimization_runtime
from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.tools.tool_args import none_default as _none_default

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


def _is_aux_workload(workload: dict[str, Any]) -> bool:
    return workload.get("workload_kind") == "aux_state"


def _benchmark_build_target(paths: dict[str, str], workload: dict[str, Any]) -> str:
    if _is_aux_workload(workload):
        return paths["aux_bench_target"]
    return paths["bench_target"]


def _aux_quick_write_frequencies(workload: dict[str, Any]) -> list[int]:
    sweep = workload.get("aux_bench_sweep") or {}
    opt_sweep = workload.get("aux_bench_sweep_optimization") or {}
    raw = opt_sweep.get("write_frequencies") or sweep.get("write_frequencies")
    if isinstance(raw, list):
        return [int(x) for x in raw]
    if isinstance(raw, str) and raw.strip():
        return [int(part.strip()) for part in raw.split(",") if part.strip()]
    return [0, 1, 10, 50]


def _aux_quick_benchmark_env(
    paths: dict[str, str],
    *,
    query: int,
    workload: dict[str, Any],
) -> dict[str, str]:
    opt = workload.get("aux_bench_sweep_optimization") or {}
    freqs = _aux_quick_write_frequencies(workload)
    return {
        paths["db_env_var"]: str(paths["dataset_db"]),
        f"ACCEL_Q{query}_BENCH_SUITE": paths.get("aux_bench_suites", "lineitem_only"),
        "ACCEL_AUX_WRITE_FREQUENCIES": ",".join(str(f) for f in freqs),
        "ACCEL_BENCH_WARMUP": str(int(opt.get("bench_warmup", 1))),
        "ACCEL_BENCH_QUERIES": str(int(opt.get("quick_bench_queries", opt.get("bench_queries", 3)))),
        "ACCEL_BENCH_REF_SAMPLES": str(int(opt.get("bench_ref_samples", 2))),
        "ACCEL_BENCH_BASE_WRITE_SAMPLES": str(int(opt.get("bench_base_write_samples", 1))),
        "ACCEL_BENCH_SKIP_PARTITION_REBUILD_CALIB": "1",
    }


def _quick_benchmark_env(
    paths: dict[str, str],
    *,
    query: int,
    workload: dict[str, Any],
    include_profile: bool,
) -> dict[str, str]:
    env = {paths["db_env_var"]: str(paths["dataset_db"])}
    if _is_aux_workload(workload):
        return _aux_quick_benchmark_env(paths, query=query, workload=workload)

    env.update(
        {
            "ACCEL_BENCH_WARMUP": "1",
            "ACCEL_BENCH_ITERS": "3",
            "ACCEL_BENCH_FILTER": f"seed_1_q{query}",
        }
    )
    if include_profile:
        env["ACCEL_BENCH_PROFILE"] = "1"
    return env


def _run_aux_quick_sweep(
    bin_path: Path,
    *,
    repo: Path,
    paths: dict[str, str],
    query: int,
    workload: dict[str, Any],
) -> tuple[list[dict[str, Any]], str]:
    freqs = _aux_quick_write_frequencies(workload)
    env = {**os.environ, **_aux_quick_benchmark_env(paths, query=query, workload=workload)}
    proc = subprocess.run(
        [str(bin_path)],
        cwd=repo,
        capture_output=True,
        text=True,
        env=env,
        check=False,
    )
    output = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode != 0:
        return [], output
    rows = parse_aux_benchmark_output(output)
    if len(rows) < len(freqs):
        return [], (
            f"expected {len(freqs)} sweep rows, got {len(rows)}\n{output[-4000:]}"
        )
    return rows, output


def _aux_quick_metrics(rows: list[dict[str, Any]]) -> dict[str, Any]:
    summary = summarize_sweep_rows(rows)
    by_freq = summary.get("by_write_frequency_pct") or {}
    zero_row = by_freq.get("0") or {}
    read_only = float(summary.get("read_only_query_speedup") or 0.0)
    composite_wl = float(summary.get("composite_workload_speedup") or 0.0)
    composite_score = float(summary.get("composite_score") or 0.0)
    setup_ms = float(zero_row.get("setup_full_build_ms") or 0.0)
    accel_hit_median_us = float(zero_row.get("accel_hit_median_us") or 0.0)

    optimization_signal: str | None = None
    if read_only < 5.0:
        optimization_signal = (
            "read_only_query_speedup is the optimization score; aggressively attack aux hit path "
            "(residual scans, map/hash chasing, decode/materialization, EmitResult hot loop)."
        )
    elif composite_wl < 1.0:
        optimization_signal = (
            "DuckDB-reference workload speedup is NOT the optimization target; keep pushing "
            "read_only_query_speedup with scalable structures (dense arrays, fusion, vectorized decode)."
        )

    return {
        "mode": "aux_quick_sweep",
        "write_frequency_points": len(rows),
        "composite_score": composite_score,
        "composite_workload_speedup": composite_wl,
        "read_only_query_speedup": read_only,
        "read_only_cold_start_workload_speedup": float(
            summary.get("read_only_cold_start_workload_speedup") or 0.0
        ),
        "setup_full_build_ms": setup_ms,
        "by_write_frequency_pct": by_freq,
        "workload_speedup": composite_wl,
        "query_only_speedup": read_only,
        "cold_start_workload_speedup": float(
            zero_row.get("cold_start_workload_speedup") or 0.0
        ),
        "accel_hit_median_us": accel_hit_median_us,
        "accel_hit_rate": float(zero_row.get("accel_hit_rate") or 0.0),
        "speedup_median": read_only,
        "optimization_score": read_only,
        "reference_median_us": float(zero_row.get("base_kernel_median_us") or 0.0),
        "accelerator_median_us": accel_hit_median_us,
        "optimization_signal": optimization_signal,
        "aux_summary": summary,
    }


def _format_quick_benchmark_result(metrics: dict[str, Any], *, aux: bool) -> str:
    if aux:
        payload: dict[str, Any] = {
            "mode": metrics.get("mode"),
            "write_frequency_points": metrics.get("write_frequency_points"),
            "composite_score": metrics.get("composite_score"),
            "composite_workload_speedup": metrics.get("composite_workload_speedup"),
            "read_only_query_speedup": metrics.get("read_only_query_speedup"),
            "read_only_cold_start_workload_speedup": metrics.get(
                "read_only_cold_start_workload_speedup"
            ),
            "setup_full_build_ms": metrics.get("setup_full_build_ms"),
            "by_write_frequency_pct": metrics.get("by_write_frequency_pct"),
            "accel_hit_median_us": metrics.get("accel_hit_median_us"),
            "optimization_signal": metrics.get("optimization_signal"),
            "speedup_median": metrics.get("speedup_median"),
        }
        # Drop null-only guidance field for cleaner JSON.
        if payload["optimization_signal"] is None:
            payload.pop("optimization_signal")
    else:
        payload = {
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
        }
    return json.dumps(payload, indent=2)


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

    def run_quick_benchmark(include_profile: bool | None = True) -> str:
        """Run a fast benchmark aligned with gate scoring and return compact metrics."""
        include_profile = bool(_none_default(include_profile, True))
        paths = resolve_workload_paths(ctx.pipeline_config, ctx.workload, ctx.query)
        aux = _is_aux_workload(ctx.workload)
        bench_target = _benchmark_build_target(paths, ctx.workload)
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

        if aux:
            rows, output = _run_aux_quick_sweep(
                bin_path,
                repo=repo,
                paths=paths,
                query=ctx.query,
                workload=ctx.workload,
            )
            if not rows:
                return f"ERROR: aux quick sweep produced no CSV rows\n{output[-4000:]}"
            metrics = _aux_quick_metrics(rows)
        else:
            env = _quick_benchmark_env(
                paths,
                query=ctx.query,
                workload=ctx.workload,
                include_profile=include_profile,
            )
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
        return _format_quick_benchmark_result(metrics, aux=aux)

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
                    _benchmark_build_target(
                        resolve_workload_paths(ctx.pipeline_config, ctx.workload, ctx.query),
                        ctx.workload,
                    ),
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
