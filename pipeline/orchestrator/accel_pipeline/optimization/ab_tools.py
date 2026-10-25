"""A/B benchmark tools for within-iteration optimization trials."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from accel_pipeline.pack.core_budget import build_jobs
from accel_pipeline.optimization.aux_bench import parse_aux_benchmark_output, summarize_sweep_rows
from accel_pipeline.optimization.aux_profile import write_aux_profile_artifacts
from accel_pipeline.optimization.base_bench import parse_benchmark_log
from accel_pipeline.build_stamp import build_lock
from accel_pipeline.config import (
    PipelineConfig,
    resolve_optimization_edit_paths,
    resolve_workload_paths,
)
from accel_pipeline.optimization.kernel_snapshot import resolve_snapshot_paths
from accel_pipeline.layout import query_dir
from accel_pipeline.optimization.measurement import check_measurement
from accel_pipeline.pack.measurement_lease import lease_note, noise_warning, run_measured
from accel_pipeline.optimization.checkpoint import load_best_valid_baseline
from accel_pipeline.optimization.eval import aux_optimization_score, improvement_ratio
from accel_pipeline.optimization.saturation import profile_guidance_error, record_trial_protocol
from accel_pipeline.optimization.runtime import record_ab_trial, refresh_optimization_runtime
from accel_pipeline.optimization.scoreboard import (
    build_scoreboard,
    primary_score_from_metrics,
    write_scoreboard,
)
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
    attempt: int | None = None


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


QUICK_TIER_DEFAULTS = {
    "write_frequencies": [0],
    "bench_warmup": 1,
    "bench_queries": 5,
    "bench_ref_samples": 1,
    "bench_base_write_samples": 1,
    "bench_timeout_sec": 120,
}


def _quick_tier_settings(workload: dict[str, Any]) -> dict[str, Any]:
    """aux_bench_sweep_quick: the repetitions of an agent's direction-only bench call."""
    return {**QUICK_TIER_DEFAULTS, **(workload.get("aux_bench_sweep_quick") or {})}


def _aux_quick_write_frequencies(
    workload: dict[str, Any],
    *,
    art_dir: Path | None = None,
    optimization_iter: int | None = None,
    attempt: int | None = None,
    acceptance: bool = True,
) -> list[int]:
    from accel_pipeline.optimization.aux_write_freqs import resolve_optimization_write_frequencies

    if not acceptance:
        return [int(f) for f in _quick_tier_settings(workload)["write_frequencies"]]
    return resolve_optimization_write_frequencies(
        workload,
        art_dir=art_dir,
        optimization_iter=optimization_iter,
        attempt=attempt,
        persist=art_dir is not None,
    )


def _aux_quick_benchmark_env(
    paths: dict[str, str],
    *,
    query: int,
    workload: dict[str, Any],
    include_profile: bool = False,
    art_dir: Path | None = None,
    optimization_iter: int | None = None,
    attempt: int | None = None,
    freqs: list[int] | None = None,
    acceptance: bool = True,
) -> dict[str, str]:
    opt = workload.get("aux_bench_sweep_optimization") or {}
    resolved = freqs if freqs is not None else _aux_quick_write_frequencies(
        workload,
        art_dir=art_dir,
        optimization_iter=optimization_iter,
        attempt=attempt,
        acceptance=acceptance,
    )
    if acceptance:
        reps = {
            "ACCEL_BENCH_WARMUP": int(opt.get("bench_warmup", 1)),
            "ACCEL_BENCH_QUERIES": int(opt.get("quick_bench_queries", opt.get("bench_queries", 3))),
            "ACCEL_BENCH_REF_SAMPLES": int(opt.get("bench_ref_samples", 2)),
            "ACCEL_BENCH_BASE_WRITE_SAMPLES": int(opt.get("bench_base_write_samples", 1)),
        }
    else:
        quick = _quick_tier_settings(workload)
        reps = {
            "ACCEL_BENCH_WARMUP": int(quick["bench_warmup"]),
            "ACCEL_BENCH_QUERIES": int(quick["bench_queries"]),
            "ACCEL_BENCH_REF_SAMPLES": int(quick["bench_ref_samples"]),
            "ACCEL_BENCH_BASE_WRITE_SAMPLES": int(quick["bench_base_write_samples"]),
        }
    env = {
        paths["db_env_var"]: str(paths["dataset_db"]),
        f"ACCEL_Q{query}_BENCH_SUITE": paths.get("aux_bench_suites", "lineitem_only"),
        "ACCEL_AUX_WRITE_FREQUENCIES": ",".join(str(f) for f in resolved),
        **{name: str(value) for name, value in reps.items()},
        "ACCEL_BENCH_SKIP_PARTITION_REBUILD_CALIB": "1",
    }
    if include_profile:
        # Enables optional agent instrumentation gated on this env var.
        env["ACCEL_AUX_CUSTOM_PROFILE"] = "1"
    return env


def _quick_benchmark_env(
    paths: dict[str, str],
    *,
    query: int,
    workload: dict[str, Any],
    include_profile: bool,
    art_dir: Path | None = None,
    optimization_iter: int | None = None,
    attempt: int | None = None,
    acceptance: bool = True,
) -> dict[str, str]:
    env = {paths["db_env_var"]: str(paths["dataset_db"])}
    if _is_aux_workload(workload):
        return _aux_quick_benchmark_env(
            paths,
            query=query,
            workload=workload,
            include_profile=include_profile,
            art_dir=art_dir,
            optimization_iter=optimization_iter,
            attempt=attempt,
            acceptance=acceptance,
        )

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


def _editable_rel_paths(ctx: AbToolContext) -> list[str]:
    edit = resolve_optimization_edit_paths(ctx.pipeline_config, ctx.workload, ctx.query)
    return [p for p in edit.get("all_paths") or [] if p]


def _reference_score_for_trial(ctx: AbToolContext) -> float:
    """Authoritative compare-against score: best_valid only (stable scoreboard)."""
    art = _art(ctx)
    aux = _is_aux_workload(ctx.workload)
    board = build_scoreboard(art, is_aux=aux)
    write_scoreboard(art, board)
    if board.get("best_valid_score"):
        return float(board["best_valid_score"])
    best = load_best_valid_baseline(art)
    if best.get("score") is not None:
        try:
            return float(best["score"])
        except (TypeError, ValueError):
            pass
    metrics = dict(best.get("metrics") or {})
    if metrics:
        return primary_score_from_metrics(metrics, is_aux=aux)
    # Last resort: fair sweep / kernel snapshot — still better than a noisy local baseline.
    if aux:
        fair_path = art / "aux_bench_summary.json"
        if fair_path.is_file():
            try:
                fair = json.loads(fair_path.read_text(encoding="utf-8"))
                return primary_score_from_metrics(fair, is_aux=True)
            except json.JSONDecodeError:
                pass
    manifest_path = art / "best_kernel_snapshot" / "manifest.json"
    if manifest_path.is_file():
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            return float(manifest.get("speedup_median") or 0.0)
        except (json.JSONDecodeError, TypeError, ValueError):
            return 0.0
    return 0.0


def _run_bench(cmd: list[str], *, acceptance: bool, **kwargs) -> subprocess.CompletedProcess:
    """Acceptance runs are measurements: they queue for the measurement lease and run on its CPUs. The quick
    tier (direction only) runs at once, on the worker's own CPUs."""
    if acceptance:
        return run_measured(cmd, reason="run_quick_benchmark acceptance", **kwargs)
    return subprocess.run(cmd, **kwargs)


def _run_aux_quick_sweep(
    bin_path: Path,
    *,
    repo: Path,
    paths: dict[str, str],
    query: int,
    workload: dict[str, Any],
    include_profile: bool = False,
    art_dir: Path | None = None,
    optimization_iter: int | None = None,
    attempt: int | None = None,
    acceptance: bool = True,
) -> tuple[list[dict[str, Any]], str]:
    freqs = _aux_quick_write_frequencies(
        workload,
        art_dir=art_dir,
        optimization_iter=optimization_iter,
        attempt=attempt,
        acceptance=acceptance,
    )
    env = {
        **os.environ,
        **_aux_quick_benchmark_env(
            paths,
            query=query,
            workload=workload,
            include_profile=include_profile,
            art_dir=art_dir,
            optimization_iter=optimization_iter,
            attempt=attempt,
            freqs=freqs,
            acceptance=acceptance,
        ),
    }
    timeout = None if acceptance else float(_quick_tier_settings(workload)["bench_timeout_sec"])
    # The bench CLI (bench_sweep_contract.md): one write-frequency point per run, as the sweep gate calls it.
    from accel_pipeline.optimization.eval import _sweep_csv

    env.pop(f"ACCEL_Q{query}_WRITE_FREQUENCY_PCT", None)
    outputs: list[str] = []
    for pct in freqs:
        try:
            proc = _run_bench(
                [str(bin_path), "--query", str(query), str(pct)],
                acceptance=acceptance,
                cwd=repo,
                capture_output=True,
                text=True,
                env=env,
                check=False,
                timeout=timeout,
            )
        except subprocess.TimeoutExpired:
            outputs.append(f"bench did not finish within {timeout:.0f}s (quick tier; acceptance=True lifts the limit)")
            return [], "\n".join(outputs)
        outputs.append((proc.stdout or "") + (proc.stderr or ""))
        if proc.returncode != 0:
            return [], "\n".join(outputs)
    output = "\n".join(outputs)
    rows = parse_aux_benchmark_output(_sweep_csv(outputs))
    if len(rows) < len(freqs):
        return [], (
            f"expected {len(freqs)} sweep rows, got {len(rows)}\n{output[-4000:]}"
        )
    return rows, output


def _aux_quick_metrics(rows: list[dict[str, Any]]) -> dict[str, Any]:
    from accel_pipeline.optimization.aux_write_freqs import aggregate_write_mix_workload_speedup

    summary = summarize_sweep_rows(rows)
    by_freq = summary.get("by_write_frequency_pct") or {}
    zero_row = by_freq.get("0") or {}
    read_only = float(summary.get("read_only_query_speedup") or 0.0)
    composite_wl = float(summary.get("composite_workload_speedup") or 0.0)
    composite_score = float(summary.get("composite_score") or 0.0)
    setup_ms = float(zero_row.get("setup_full_build_ms") or 0.0)
    accel_hit_median_us = float(zero_row.get("accel_hit_median_us") or 0.0)
    write_mix_workload_speedup, write_mix_pcts = aggregate_write_mix_workload_speedup(
        {"by_write_frequency_pct": by_freq}
    )
    write_mix_pct = write_mix_pcts[0] if len(write_mix_pcts) == 1 else 0
    mix_label = (
        f"{write_mix_pct}%"
        if len(write_mix_pcts) == 1
        else ("+".join(f"{p}%" for p in write_mix_pcts) if write_mix_pcts else "write-mix")
    )

    optimization_signal: str | None = None
    if write_mix_pcts and write_mix_workload_speedup > 0 and write_mix_workload_speedup < 1.0:
        optimization_signal = (
            f"{mix_label} write-mix workload_speedup={write_mix_workload_speedup:.2f}x "
            "(below 1x vs DuckDB) — incremental maintenance is too slow; prefer delta/partition-local "
            "apply over full rebuilds on point writes. Dual-axis accept score penalizes this cliff."
        )
    elif write_mix_pcts and write_mix_workload_speedup > 0 and read_only > 10.0 and (
        write_mix_workload_speedup < max(1.0, read_only * 0.02)
    ):
        optimization_signal = (
            f"Read-only is strong ({read_only:.1f}x) but {mix_label} write-mix "
            f"workload_speedup={write_mix_workload_speedup:.2f}x is a large maintenance cliff — "
            "optimize bulk/point incremental maintenance; dual-axis score weights write_mix^0.25."
        )
    elif read_only < 5.0:
        optimization_signal = (
            "read_only_query_speedup is weak; aggressively attack aux hit path "
            "(residual scans, map/hash chasing, decode/materialization, EmitResult hot loop)."
        )
    elif composite_wl < 1.0:
        optimization_signal = (
            "Keep pushing dual-axis score (read^0.75 * write_mix^0.25); watch write-mix health "
            f"across sampled frequencies ({mix_label})."
        )

    dual_score = aux_optimization_score(
        {
            "read_only_query_speedup": read_only,
            "write_mix_workload_speedup": write_mix_workload_speedup,
            "write_mix_pcts": write_mix_pcts,
            "by_write_frequency_pct": by_freq,
            "accel_hit_median_us": accel_hit_median_us,
        }
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
        "write_mix_pct": write_mix_pct,
        "write_mix_pcts": write_mix_pcts,
        "write_mix_workload_speedup": write_mix_workload_speedup,
        "workload_speedup": composite_wl,
        "query_only_speedup": read_only,
        "cold_start_workload_speedup": float(
            zero_row.get("cold_start_workload_speedup") or 0.0
        ),
        "accel_hit_median_us": accel_hit_median_us,
        "accel_hit_rate": float(zero_row.get("accel_hit_rate") or 0.0),
        "speedup_median": dual_score if dual_score > 0 else read_only,
        "optimization_score": dual_score if dual_score > 0 else read_only,
        "reference_median_us": float(zero_row.get("ref_query_median_us") or 0.0),
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
            "read_speedup_vs_engine": metrics.get("read_only_query_speedup"),
            "read_only_cold_start_workload_speedup": metrics.get(
                "read_only_cold_start_workload_speedup"
            ),
            "setup_full_build_ms": metrics.get("setup_full_build_ms"),
            "write_mix_pct": metrics.get("write_mix_pct"),
            "write_mix_pcts": metrics.get("write_mix_pcts"),
            "write_mix_workload_speedup": metrics.get("write_mix_workload_speedup"),
            "by_write_frequency_pct": metrics.get("by_write_frequency_pct"),
            "optimization_score": metrics.get("optimization_score"),
            "speedup_median": metrics.get("speedup_median"),
            "optimization_signal": metrics.get("optimization_signal"),
        }
        # Drop null-only guidance field for cleaner JSON.
        if payload["optimization_signal"] is None:
            payload.pop("optimization_signal")
        if payload.get("optimization_score") is None:
            payload.pop("optimization_score")
        if not payload.get("write_mix_pcts"):
            payload.pop("write_mix_pcts", None)
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
    def begin_ab_trial(
        trial_id: str, hypothesis: str, hot_spot: str = "", expected_gain: str = "", strategy: str = ""
    ) -> str:
        """Snapshot current kernel files before an A/B trial that targets a profiled hot spot."""
        if _read_active_trial(ctx):
            return "ERROR: an A/B trial is already active; call end_ab_trial first."
        guidance_error = profile_guidance_error(
            _art(ctx), hot_spot=hot_spot, expected_gain=expected_gain, strategy=strategy
        )
        if guidance_error:
            return f"ERROR: a trial starts from the profile: {guidance_error}"
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
        ref_score = _reference_score_for_trial(ctx)
        board = build_scoreboard(_art(ctx), is_aux=_is_aux_workload(ctx.workload))
        write_scoreboard(_art(ctx), board)
        record_trial_protocol(
            _art(ctx), trial_id=trial_id, hot_spot=hot_spot, expected_gain=expected_gain, strategy=strategy
        )
        _write_active_trial(
            ctx,
            {
                "trial_id": trial_id,
                "hypothesis": hypothesis,
                "files": copied,
                "reference_score": ref_score,
                "compare_against": "best_valid_score",
                "scoreboard": board,
            },
        )
        return json.dumps(
            {
                "status": "checkpointed",
                "trial_id": trial_id,
                "files": copied,
                "reference_score": ref_score,
                "compare_against": "best_valid_score",
                "scoreboard": board,
                "guidance": (
                    "A/B keep/regress vs scoreboard.best_valid_score only. "
                    "After edits: quick_check, then run_quick_benchmark(acceptance=True), then end_ab_trial."
                ),
            },
            indent=2,
        )

    def run_quick_benchmark(include_profile: bool | None = True, acceptance: bool | None = False) -> str:
        """Run a benchmark and return compact metrics. Default: the quick tier (a few queries, read
        axis only: direction, not a measurement). acceptance=True: the full optimization tier
        the accept/reject decision uses."""
        include_profile = bool(_none_default(include_profile, True))
        acceptance = bool(_none_default(acceptance, False))
        paths = resolve_workload_paths(ctx.pipeline_config, ctx.workload, ctx.query)
        from accel_pipeline.pack.worker_isolation import ensure_private_dataset_db

        try:
            private_db = ensure_private_dataset_db(
                ctx.pipeline_config, paths["dataset_db"], query=ctx.query
            )
            paths = {**paths, "dataset_db": str(private_db)}
        except Exception as exc:  # noqa: BLE001
            return f"ERROR: private dataset db setup failed: {exc}"
        aux = _is_aux_workload(ctx.workload)
        invalid: list[str] = []
        bench_target = _benchmark_build_target(paths, ctx.workload)
        build_dir = ctx.pipeline_config.build_dir
        repo = ctx.pipeline_config.repo_root
        bin_path = build_dir / bench_target

        with build_lock(build_dir):
            build = subprocess.run(
                ["cmake", "--build", str(build_dir), "--target", bench_target, "-j", str(build_jobs())],
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
                include_profile=include_profile,
                art_dir=_art(ctx),
                optimization_iter=ctx.optimization_iter,
                attempt=ctx.attempt,
                acceptance=acceptance,
            )
            if not rows:
                return f"ERROR: aux quick sweep produced no CSV rows\n{output[-4000:]}"
            metrics = _aux_quick_metrics(rows)
            invalid = [
                line
                for failure in check_measurement(
                    rows,
                    workload=ctx.workload,
                    phase="attempt",
                    config=ctx.pipeline_config.measurement,
                    bench_binary=bin_path,
                    query_dir=repo / query_dir(str(ctx.workload.get("benchmark_name", "tpch")), ctx.query),
                )
                for line in failure.diagnosis
            ]
            write_aux_profile_artifacts(
                _art(ctx),
                metrics,
                source="run_quick_benchmark",
                bench_output=output,
                repo_root=ctx.pipeline_config.repo_root,
                editable_rel_paths=_editable_rel_paths(ctx),
            )
        else:
            env = _quick_benchmark_env(
                paths,
                query=ctx.query,
                workload=ctx.workload,
                include_profile=include_profile,
            )
            proc = _run_bench(
                [str(bin_path)],
                acceptance=acceptance,
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

        active = _read_active_trial(ctx) if acceptance else {}
        if active and invalid:
            # A bench that did not measure the accelerator voids the trial's metrics: end_ab_trial then discards.
            for key in ("last_metrics", "last_score", "last_improvement_pct"):
                active.pop(key, None)
            _write_active_trial(ctx, active)
            active = {}
        if active:
            active["last_metrics"] = metrics
            cand = (
                aux_optimization_score(metrics, ctx.workload)
                if aux
                else float(metrics.get("speedup_median") or 0.0)
            )
            ref = float(active.get("reference_score") or 0.0)
            active["last_improvement_pct"] = improvement_ratio(ref, cand) if ref > 0 else None
            active["last_score"] = cand
            _write_active_trial(ctx, active)

        refresh_optimization_runtime(
            ctx.pipeline_config,
            ctx.workload,
            ctx.query,
            optimization_iter=ctx.optimization_iter,
        )
        board = build_scoreboard(_art(ctx), is_aux=aux, last_metrics=metrics)
        write_scoreboard(_art(ctx), board)
        formatted = _format_quick_benchmark_result(metrics, aux=aux)
        try:
            payload = json.loads(formatted)
        except json.JSONDecodeError:
            return formatted
        payload["scoreboard"] = board
        payload["tier"] = (
            "acceptance"
            if acceptance
            else "quick: direction only, never kept or discarded on this; end_ab_trial(keep=True) needs an acceptance=True run"
        )
        if lease := lease_note(output):
            payload["measurement_lease"] = {**lease, "note": f"waited {lease['waited_sec']} s for the measurement lease"}
        elif not acceptance and (noise := noise_warning(ctx.pipeline_config.repair.load_factor)):
            payload["measurement_noise"] = noise
        if invalid:
            payload["measurement_invalid"] = invalid
            payload["ab_signal"] = "invalid_measurement"
        elif active and active.get("last_improvement_pct") is not None:
            # Surface A/B signal explicitly so agents cannot ignore regressions.
            payload["vs_reference_score"] = active.get("reference_score")
            payload["vs_best_valid_score"] = board.get("best_valid_score")
            payload["improvement_pct_vs_reference"] = active.get("last_improvement_pct")
            payload["ab_signal"] = (
                "improve"
                if float(active["last_improvement_pct"]) > 0.5
                else "regress"
                if float(active["last_improvement_pct"]) < -0.5
                else "flat"
            )
            payload["ab_compare_against"] = "best_valid_score"
        return json.dumps(payload, indent=2)

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
        forced_discard = False
        discard_reason = ""

        # Honor exploratory signal: never keep a measured regression vs reference.
        if keep:
            if not metrics:
                keep = False
                forced_discard = True
                discard_reason = "keep_requested_without_metrics"
            else:
                cand = (
                    aux_optimization_score(metrics, ctx.workload)
                    if _is_aux_workload(ctx.workload)
                    else float(metrics.get("speedup_median") or 0.0)
                )
                ref = float(active.get("reference_score") or _reference_score_for_trial(ctx) or 0.0)
                if ref > 0 and cand + 1e-9 < ref:
                    keep = False
                    forced_discard = True
                    discard_reason = (
                        f"exploratory_regress cand={cand:.4f} < reference={ref:.4f}"
                    )

        if not keep:
            for rel in active.get("files") or []:
                src = ckpt / rel
                if not src.is_file():
                    continue
                dest = ctx.pipeline_config.repo_root / rel
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dest)
                restored.append(rel)
            with build_lock(ctx.pipeline_config.build_dir):
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
                        str(build_jobs()),
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
        board = build_scoreboard(
            _art(ctx),
            is_aux=_is_aux_workload(ctx.workload),
            last_metrics=metrics or None,
        )
        write_scoreboard(_art(ctx), board)
        payload: dict[str, Any] = {
            "trial_id": trial_id,
            "kept": keep,
            "restored_files": restored,
            "metrics": metrics,
            "scoreboard": board,
            "compare_against": "best_valid_score",
        }
        if forced_discard:
            payload["forced_discard"] = True
            payload["discard_reason"] = discard_reason
            payload["guidance"] = (
                "Exploratory A/B showed a regression vs best_valid_score (or missing metrics). "
                "Do not yield CANDIDATE_READY for this change; form a new hypothesis. "
                "After restore, call run_profile — prior phase breakdown is stale."
            )
        elif keep:
            payload["guidance"] = (
                "Exploratory A/B improved vs best_valid_score — you may yield CANDIDATE_READY "
                "for authoritative gates. Remeasure with run_profile before the next hypothesis."
            )
        else:
            payload["guidance"] = (
                "Changes restored. Do not yield the discarded candidate; try another hypothesis. "
                "Call run_profile on the restored tree before hypothesizing again."
            )
        return json.dumps(payload, indent=2)

    return begin_ab_trial, run_quick_benchmark, end_ab_trial
