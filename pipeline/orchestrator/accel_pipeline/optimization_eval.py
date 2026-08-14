"""Deterministic optimization benchmarking and accept/reject policy."""

from __future__ import annotations

import os
import statistics
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from accel_pipeline.aux_bench import parse_aux_benchmark_output, summarize_sweep_rows
from accel_pipeline.base_bench import parse_benchmark_log
from accel_pipeline.config import PipelineConfig, resolve_workload_paths
from accel_pipeline.optimization_signals import attach_store_diagnostics, extract_store_diagnostics


@dataclass(frozen=True)
class EvalConfig:
    improvement_threshold_pct: float = 3.0
    reject_threshold_pct: float = 2.0
    quick_benchmark_samples: int = 3
    confirmation_samples: int = 3
    aux_quick_benchmark_samples: int = 1


@dataclass
class BenchmarkSample:
    metrics: dict[str, Any]
    score: float


@dataclass
class MultiSampleBenchmark:
    samples: list[BenchmarkSample]
    median_score: float
    median_metrics: dict[str, Any]


@dataclass
class InitialScreenResult:
    verdict: str  # "reject" | "needs_confirmation"
    improvement_pct: float
    baseline_median: float
    candidate_median: float


@dataclass
class AcceptRejectResult:
    accepted: bool
    improvement_pct: float
    baseline_median: float
    candidate_median: float
    reason: str = ""


@dataclass
class AttemptOutcome:
    verdict: str  # accept | keep_explore | reject_broken | reject_regress
    improvement_pct: float
    baseline_score: float
    candidate_score: float
    diagnostic_vector: dict[str, Any]
    reason: str = ""


STRUCTURAL_DIAGNOSTIC_AXES: tuple[str, ...] = (
    "validity_slots_scanned_median",
    "validity_scan_us_median",
    "partitions_read_per_query_median",
    "state_bytes_read_median",
    "valid_partitions",
    "total_slots",
)


def _structural_progress_pct(
    baseline_diag: dict[str, Any],
    candidate_diag: dict[str, Any],
) -> float:
    """Positive when candidate reduces measured structural work on any axis."""
    improvements: list[float] = []
    lower_is_better = {
        "validity_slots_scanned_median",
        "validity_scan_us_median",
        "partitions_read_per_query_median",
        "state_bytes_read_median",
        "valid_partitions",
        "total_slots",
    }
    for key in STRUCTURAL_DIAGNOSTIC_AXES:
        base_val = baseline_diag.get(key)
        cand_val = candidate_diag.get(key)
        if not isinstance(base_val, (int, float)) or not isinstance(cand_val, (int, float)):
            continue
        if base_val <= 0:
            continue
        if key in lower_is_better:
            delta_pct = 100.0 * (base_val - cand_val) / base_val
        else:
            delta_pct = 100.0 * (cand_val - base_val) / base_val
        if delta_pct > 0:
            improvements.append(delta_pct)
    return max(improvements) if improvements else 0.0


def evaluate_attempt_outcome(
    *,
    baseline_score: float,
    candidate_score: float,
    baseline_metrics: dict[str, Any],
    candidate_metrics: dict[str, Any],
    config: EvalConfig,
    correctness_ok: bool,
    structural_progress_threshold_pct: float = 5.0,
    severe_regress_threshold_pct: float = 10.0,
) -> AttemptOutcome:
    """Multi-axis verdict: accept / keep_explore / reject_broken / reject_regress."""
    baseline_diag = extract_store_diagnostics(baseline_metrics)
    candidate_diag = extract_store_diagnostics(candidate_metrics)
    diagnostic_vector = {
        "baseline": baseline_diag,
        "candidate": candidate_diag,
        "read_only_query_speedup_baseline": baseline_metrics.get("read_only_query_speedup"),
        "read_only_query_speedup_candidate": candidate_metrics.get("read_only_query_speedup"),
        "accel_hit_median_us_baseline": baseline_metrics.get("accel_hit_median_us"),
        "accel_hit_median_us_candidate": candidate_metrics.get("accel_hit_median_us"),
        "accel_hit_rate_candidate": candidate_metrics.get("accel_hit_rate"),
    }
    improvement_pct = improvement_ratio(baseline_score, candidate_score)

    if not correctness_ok:
        return AttemptOutcome(
            verdict="reject_broken",
            improvement_pct=improvement_pct,
            baseline_score=baseline_score,
            candidate_score=candidate_score,
            diagnostic_vector=diagnostic_vector,
            reason="correctness_probe_failed",
        )

    hit_rate = float(candidate_metrics.get("accel_hit_rate") or 0.0)
    if hit_rate > 0 and hit_rate < 0.5:
        return AttemptOutcome(
            verdict="reject_broken",
            improvement_pct=improvement_pct,
            baseline_score=baseline_score,
            candidate_score=candidate_score,
            diagnostic_vector=diagnostic_vector,
            reason=f"accel_hit_rate anomaly: {hit_rate:.3f}",
        )

    if improvement_pct >= config.improvement_threshold_pct:
        return AttemptOutcome(
            verdict="accept",
            improvement_pct=improvement_pct,
            baseline_score=baseline_score,
            candidate_score=candidate_score,
            diagnostic_vector=diagnostic_vector,
            reason="read_improvement_above_accept_threshold",
        )

    if improvement_pct < -severe_regress_threshold_pct:
        return AttemptOutcome(
            verdict="reject_regress",
            improvement_pct=improvement_pct,
            baseline_score=baseline_score,
            candidate_score=candidate_score,
            diagnostic_vector=diagnostic_vector,
            reason="read_performance_severely_regressed",
        )

    structural_pct = _structural_progress_pct(baseline_diag, candidate_diag)
    if structural_pct >= structural_progress_threshold_pct and improvement_pct >= -config.reject_threshold_pct:
        return AttemptOutcome(
            verdict="keep_explore",
            improvement_pct=improvement_pct,
            baseline_score=baseline_score,
            candidate_score=candidate_score,
            diagnostic_vector=diagnostic_vector,
            reason=f"structural_progress {structural_pct:.1f}% with correctness held",
        )

    if improvement_pct < config.reject_threshold_pct:
        return AttemptOutcome(
            verdict="reject_regress",
            improvement_pct=improvement_pct,
            baseline_score=baseline_score,
            candidate_score=candidate_score,
            diagnostic_vector=diagnostic_vector,
            reason="below_reject_threshold_without_structural_progress",
        )

    return AttemptOutcome(
        verdict="reject_regress",
        improvement_pct=improvement_pct,
        baseline_score=baseline_score,
        candidate_score=candidate_score,
        diagnostic_vector=diagnostic_vector,
        reason="no_accept_or_keep_explore_criteria_met",
    )


def is_aux_workload(workload: dict[str, Any]) -> bool:
    return workload.get("workload_kind") == "aux_state"


def aux_optimization_score(
    metrics: dict[str, Any],
    workload: dict[str, Any] | None = None,
) -> float:
    """Primary aux optimization signal: read-path speedup, or accel hit time when DuckDB ref skipped."""
    metric = "read_only_query_speedup"
    if workload:
        opt = workload.get("aux_bench_sweep_optimization") or {}
        metric = str(
            opt.get("optimization_score_metric")
            or workload.get("optimization_score_metric")
            or metric
        )
    for key in (metric, "read_only_query_speedup", "composite_query_speedup", "composite_score"):
        raw = metrics.get(key)
        if raw is not None and float(raw) > 0:
            return float(raw)
    hit_us = float(metrics.get("accel_hit_median_us") or 0.0)
    if hit_us > 0:
        # Higher score = faster read path when DuckDB reference is skipped in quick sweep.
        return 1.0e9 / hit_us
    return float(metrics.get("speedup_median") or 0.0)


def score_metrics(
    metrics: dict[str, Any],
    *,
    is_aux: bool,
    workload: dict[str, Any] | None = None,
) -> float:
    if is_aux:
        return aux_optimization_score(metrics, workload)
    return float(metrics.get("speedup_median") or 0.0)


def improvement_ratio(baseline_score: float, candidate_score: float) -> float:
    if baseline_score <= 0:
        return 100.0 if candidate_score > 0 else 0.0
    return 100.0 * (candidate_score - baseline_score) / baseline_score


def _aux_write_frequencies(workload: dict[str, Any]) -> list[int]:
    sweep = workload.get("aux_bench_sweep", {}) or {}
    opt_sweep = workload.get("aux_bench_sweep_optimization", {}) or {}
    raw = opt_sweep.get("write_frequencies") or sweep.get("write_frequencies")
    if isinstance(raw, list):
        return [int(x) for x in raw]
    if isinstance(raw, str) and raw.strip():
        return [int(part.strip()) for part in raw.split(",") if part.strip()]
    return [0, 1, 10, 50]


def _aux_quick_env(
    paths: dict[str, str],
    *,
    query: int,
    workload: dict[str, Any],
) -> dict[str, str]:
    opt = workload.get("aux_bench_sweep_optimization", {}) or {}
    freqs = _aux_write_frequencies(workload)
    env = {
        paths["db_env_var"]: str(paths["dataset_db"]),
        f"ACCEL_Q{query}_BENCH_SUITE": paths.get("aux_bench_suites", "lineitem_only"),
        "ACCEL_AUX_WRITE_FREQUENCIES": ",".join(str(f) for f in freqs),
        "ACCEL_BENCH_WARMUP": str(int(opt.get("bench_warmup", 1))),
        "ACCEL_BENCH_QUERIES": str(int(opt.get("quick_bench_queries", opt.get("bench_queries", 3)))),
        "ACCEL_BENCH_REF_SAMPLES": str(int(opt.get("bench_ref_samples", 0))),
        "ACCEL_BENCH_BASE_WRITE_SAMPLES": str(int(opt.get("bench_base_write_samples", 0))),
        "ACCEL_BENCH_SKIP_PARTITION_REBUILD_CALIB": "1",
    }
    if opt.get("skip_duckdb_ref"):
        env["ACCEL_BENCH_SKIP_DUCKDB_REF"] = "1"
    return env


def _base_benchmark_env(
    paths: dict[str, str],
    *,
    query: int,
    include_profile: bool,
) -> dict[str, str]:
    env = {
        paths["db_env_var"]: str(paths["dataset_db"]),
        "ACCEL_BENCH_WARMUP": "1",
        "ACCEL_BENCH_ITERS": "3",
        "ACCEL_BENCH_FILTER": f"seed_1_q{query}",
    }
    if include_profile:
        env["ACCEL_BENCH_PROFILE"] = "1"
    return env


def _benchmark_target(paths: dict[str, str], workload: dict[str, Any]) -> str:
    if is_aux_workload(workload):
        return paths["aux_bench_target"]
    return paths["bench_target"]


def _log_benchmark(msg: str) -> None:
    print(f"[pipeline] {msg}", file=sys.stderr, flush=True)


def _build_benchmark(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
) -> str | None:
    paths = resolve_workload_paths(pipeline_config, workload, query)
    target = _benchmark_target(paths, workload)
    _log_benchmark(f"building benchmark target {target}")
    build = subprocess.run(
        ["cmake", "--build", str(pipeline_config.build_dir), "--target", target, "-j", "4"],
        cwd=pipeline_config.repo_root,
        capture_output=True,
        text=True,
        check=False,
    )
    if build.returncode != 0:
        return f"build failed\n{(build.stdout or '') + (build.stderr or '')[-2000:]}"
    return None


def _metrics_from_aux_rows(rows: list[dict[str, Any]]) -> dict[str, Any]:
    summary = summarize_sweep_rows(rows)
    by_freq = summary.get("by_write_frequency_pct") or {}
    zero_row = by_freq.get("0") or (rows[0] if rows else {})
    composite_wl = float(summary.get("composite_workload_speedup") or 0.0)
    read_only = float(summary.get("read_only_query_speedup") or 0.0)
    composite_score = float(summary.get("composite_score") or 0.0)
    accel_hit_median_us = float(
        zero_row.get("accel_hit_median_us") or rows[0].get("accel_hit_median_us", 0.0) if rows else 0.0
    )
    accel_hit_rate = float(zero_row.get("accel_hit_rate") or 0.0)
    metrics = {
        "mode": "aux_quick_sweep",
        "write_frequency_points": len(rows),
        "composite_score": composite_score,
        "composite_workload_speedup": composite_wl,
        "composite_query_speedup": float(summary.get("composite_query_speedup") or read_only),
        "read_only_query_speedup": read_only,
        "speedup_median": read_only if read_only > 0 else (
            (1.0e9 / accel_hit_median_us) if accel_hit_median_us > 0 else 0.0
        ),
        "optimization_score": read_only if read_only > 0 else (
            (1.0e9 / accel_hit_median_us) if accel_hit_median_us > 0 else 0.0
        ),
        "accel_hit_median_us": accel_hit_median_us,
        "accel_hit_rate": accel_hit_rate,
        "setup_full_build_ms": float(zero_row.get("setup_full_build_ms") or 0.0),
        "aux_summary": summary,
    }
    for key in (
        "valid_partitions",
        "total_slots",
        "validity_slots_scanned_median",
        "validity_scan_us_median",
        "partitions_read_per_query_median",
        "state_bytes_read_median",
        "store_execute_calls",
    ):
        if key in zero_row:
            metrics[key] = zero_row[key]
    return attach_store_diagnostics(metrics)


def _run_aux_sweep_once(
    bin_path: str,
    *,
    repo: Path,
    paths: dict[str, str],
    query: int,
    workload: dict[str, Any],
) -> tuple[dict[str, Any] | None, str]:
    """One benchmark process: multi-frequency sweep via ACCEL_AUX_WRITE_FREQUENCIES."""
    freqs = _aux_write_frequencies(workload)
    env = {**os.environ, **_aux_quick_env(paths, query=query, workload=workload)}
    _log_benchmark(
        f"aux quick sweep (1 process): freqs={freqs} "
        f"queries={env.get('ACCEL_BENCH_QUERIES')} warmup={env.get('ACCEL_BENCH_WARMUP')}"
    )
    proc = subprocess.run(
        [bin_path],
        cwd=repo,
        capture_output=True,
        text=True,
        env=env,
        check=False,
    )
    output = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode != 0:
        rc = proc.returncode
        if rc == -9 or rc == 137:
            hint = (
                "benchmark subprocess killed (SIGKILL); likely OOM from a bad patch "
                "or stale binary after a reverted patch"
            )
            return None, hint + (f"\n{output[-2000:]}" if output.strip() else "")
        return None, output or f"benchmark exited with code {rc}"
    rows = parse_aux_benchmark_output(output)
    if len(rows) < len(freqs):
        return None, (
            f"expected {len(freqs)} sweep rows, got {len(rows)}\n{output[-4000:]}"
        )
    return _metrics_from_aux_rows(rows), output


def _run_base_once(
    bin_path: str,
    *,
    repo: Path,
    paths: dict[str, str],
    query: int,
    include_profile: bool,
) -> tuple[dict[str, Any] | None, str]:
    env = _base_benchmark_env(paths, query=query, include_profile=include_profile)
    proc = subprocess.run(
        [bin_path],
        cwd=repo,
        capture_output=True,
        text=True,
        env={**os.environ, **env},
        check=False,
    )
    output = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode != 0:
        return None, output
    metrics = parse_benchmark_log(output)
    if not metrics:
        return None, output
    return metrics, output


def run_single_benchmark(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    *,
    include_profile: bool = False,
) -> tuple[dict[str, Any] | None, str]:
    """Run one benchmark invocation; return (metrics, output_or_error)."""
    paths = resolve_workload_paths(pipeline_config, workload, query)
    target = _benchmark_target(paths, workload)
    bin_path = str(pipeline_config.build_dir / target)
    build_err = _build_benchmark(pipeline_config, workload, query)
    if build_err:
        return None, build_err
    if is_aux_workload(workload):
        return _run_aux_sweep_once(
            bin_path,
            repo=pipeline_config.repo_root,
            paths=paths,
            query=query,
            workload=workload,
        )
    return _run_base_once(
        bin_path,
        repo=pipeline_config.repo_root,
        paths=paths,
        query=query,
        include_profile=include_profile,
    )


def _aux_sample_count(
    workload: dict[str, Any],
    config: EvalConfig,
    *,
    confirmation: bool,
) -> int:
    if confirmation:
        return 1
    opt = workload.get("aux_bench_sweep_optimization") or {}
    if opt.get("quick_benchmark_samples") is not None:
        return max(1, int(opt["quick_benchmark_samples"]))
    return max(1, config.aux_quick_benchmark_samples)


def run_multi_sample_benchmark(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    *,
    samples: int = 3,
    include_profile: bool = False,
    confirmation: bool = False,
) -> MultiSampleBenchmark | None:
    aux = is_aux_workload(workload)
    if aux:
        sample_count = _aux_sample_count(
            workload,
            eval_config_from_pipeline(pipeline_config),
            confirmation=confirmation,
        )
    else:
        sample_count = samples if not confirmation else max(1, samples)
    collected: list[BenchmarkSample] = []
    for sample_idx in range(sample_count):
        if aux:
            _log_benchmark(f"aux benchmark sample {sample_idx + 1}/{sample_count}")
        metrics, err = run_single_benchmark(
            pipeline_config,
            workload,
            query,
            include_profile=include_profile and not collected,
        )
        if not metrics:
            if not collected:
                _log_benchmark(f"benchmark failed: {(err or '')[-500:]}")
                return None
            continue
        collected.append(
            BenchmarkSample(
                metrics=metrics,
                score=score_metrics(metrics, is_aux=aux, workload=workload),
            )
        )
    if not collected:
        return None
    scores = [s.score for s in collected]
    median_score = float(statistics.median(scores))
    median_idx = min(range(len(scores)), key=lambda i: abs(scores[i] - median_score))
    return MultiSampleBenchmark(
        samples=collected,
        median_score=median_score,
        median_metrics=collected[median_idx].metrics,
    )


def evaluate_initial_screen(
    baseline_median: float,
    candidate_median: float,
    config: EvalConfig,
) -> InitialScreenResult:
    improvement_pct = improvement_ratio(baseline_median, candidate_median)
    if improvement_pct < config.reject_threshold_pct:
        return InitialScreenResult(
            verdict="reject",
            improvement_pct=improvement_pct,
            baseline_median=baseline_median,
            candidate_median=candidate_median,
        )
    return InitialScreenResult(
        verdict="needs_confirmation",
        improvement_pct=improvement_pct,
        baseline_median=baseline_median,
        candidate_median=candidate_median,
    )


def evaluate_confirmation(
    baseline_median: float,
    confirmed_median: float,
    config: EvalConfig,
) -> AcceptRejectResult:
    """Terminal binary decision after confirmation run."""
    improvement_pct = improvement_ratio(baseline_median, confirmed_median)
    if improvement_pct >= config.improvement_threshold_pct:
        return AcceptRejectResult(
            accepted=True,
            improvement_pct=improvement_pct,
            baseline_median=baseline_median,
            candidate_median=confirmed_median,
        )
    return AcceptRejectResult(
        accepted=False,
        improvement_pct=improvement_pct,
        baseline_median=baseline_median,
        candidate_median=confirmed_median,
        reason=f"confirmation improvement {improvement_pct:.2f}% < {config.improvement_threshold_pct}%",
    )


def eval_config_from_pipeline(pipeline_config: PipelineConfig) -> EvalConfig:
    aux_samples = 1
    try:
        from accel_pipeline.config import load_aux_state_defaults

        raw_opt = load_aux_state_defaults(pipeline_config.repo_root).get(
            "aux_bench_sweep_optimization", {}
        )
        if isinstance(raw_opt, dict) and raw_opt.get("quick_benchmark_samples") is not None:
            aux_samples = int(raw_opt["quick_benchmark_samples"])
    except Exception:  # noqa: BLE001
        pass
    return EvalConfig(
        improvement_threshold_pct=float(
            getattr(pipeline_config, "optimization_improvement_threshold_pct", 3.0)
        ),
        reject_threshold_pct=float(getattr(pipeline_config, "optimization_reject_threshold_pct", 2.0)),
        quick_benchmark_samples=int(
            getattr(pipeline_config, "optimization_quick_benchmark_samples", 3)
        ),
        confirmation_samples=int(getattr(pipeline_config, "optimization_confirmation_samples", 3)),
        aux_quick_benchmark_samples=aux_samples,
    )
