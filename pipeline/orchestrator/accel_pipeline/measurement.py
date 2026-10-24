"""Is a benchmark measuring the accelerator, and are its numbers plausible?"""

from __future__ import annotations

import math
import statistics
from pathlib import Path
from typing import Any

from accel_pipeline.aux_bench import summarize_sweep_rows
from accel_pipeline.config import MeasurementConfig
from accel_pipeline.failure import Failure

_SPEEDUP_KEYS = (
    "workload_speedup",
    "query_only_speedup",
    "cold_start_workload_speedup",
    "blended_query_speedup",
    "speedup_median",
)
_SLOWER_THAN_FALLBACK_FACTOR = 10.0
_READ_AXIS_TOLERANCE = (0.9, 1.1)  # the bench rounds its microsecond columns
_SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".inc"}
_OTHER_TARGET_DIRS = {"tests", "bench", "config"}  # edits there do not relink the bench binary
_EVIDENCE_KEYS = (
    "write_frequency_pct", "num_queries", "accel_hit_rate", "accel_fallback_reads", "state_builds",
    "accel_hit_median_us", "fallback_median_us", "ref_query_median_us", "query_only_speedup",
)


def check_measurement(
    rows_or_metrics: Any,
    *,
    workload: dict,
    phase: str,
    baseline: dict | None = None,
    config: MeasurementConfig | None = None,
    bench_binary: Path | None = None,
    query_dir: Path | None = None,
) -> list[Failure]:
    """Failures (kind="measurement", non-empty diagnosis) when the signal is untrustworthy; [] when it is.

    rows_or_metrics: aux_bench.parse_sweep_csv rows, a metrics dict (by_write_frequency_pct or flat),
    or a MultiSampleBenchmark. phase: "baseline" | "attempt" | "gate".
    """
    cfg = config or MeasurementConfig()
    if not cfg.enabled:
        return []
    samples = getattr(rows_or_metrics, "samples", None) or []
    points = _points(getattr(rows_or_metrics, "median_metrics", rows_or_metrics))
    diagnosis = [
        *_hit_diagnosis(points, cfg, _points(baseline)),
        *_fallback_diagnosis(points, cfg),
        *_built_diagnosis(points, cfg),
        *_timing_diagnosis(points, cfg),
        *_spread_diagnosis([s.score for s in samples], cfg),
        *_stale_diagnosis(bench_binary, query_dir),
    ]
    if not diagnosis:
        return []
    return [
        Failure(
            kind="measurement",
            stage="optimization" if phase in ("baseline", "attempt") else phase,
            source=f"bench_{phase}",
            summary=f"{workload.get('name') or 'workload'}: the benchmark signal is not trustworthy",
            evidence="\n".join(_describe(p) for p in points),
            diagnosis=diagnosis,
        )
    ]


def _points(data: Any) -> list[dict[str, Any]]:
    if isinstance(data, list):
        data = summarize_sweep_rows(data)
    if not isinstance(data, dict) or not data:
        return []
    by_freq = data.get("by_write_frequency_pct")
    if isinstance(by_freq, dict) and by_freq:
        return [{"write_frequency_pct": key, **point} for key, point in by_freq.items()]
    return [data]


def _num(point: dict[str, Any], key: str) -> float | None:
    try:
        return float(point[key])
    except (KeyError, TypeError, ValueError):
        return None


def _read_only(point: dict[str, Any]) -> bool:
    interval = _num(point, "write_interval_sec")
    if interval is not None and interval > 0:
        return False
    return not _num(point, "write_frequency_pct")


def _read_only_points(points: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [p for p in points if _read_only(p)]


def _label(point: dict[str, Any]) -> str:
    return f"write frequency {int(_num(point, 'write_frequency_pct') or 0)}%"


def _hit_diagnosis(
    points: list[dict[str, Any]], cfg: MeasurementConfig, baseline_points: list[dict[str, Any]]
) -> list[str]:
    out = []
    for point in _read_only_points(points):
        rate = _num(point, "accel_hit_rate")
        if rate is None or rate > cfg.min_hit_rate:
            continue
        if rate == 0:
            reads = int(_num(point, "num_queries") or 0) or "the measured"
            out.append(
                f"bench measured the fallback path: 0 of {reads} reads hit the accelerator. "
                "Likely causes: query not registered / first-read build not triggered / "
                "harness uses a different SQL entry"
            )
        else:
            out.append(
                f"accel_hit_rate {rate:.3f} at write frequency 0 is not above {cfg.min_hit_rate}: "
                "most reads were answered by the fallback, so the speedup does not describe the accelerator"
            )
        was = [r for p in _read_only_points(baseline_points) if (r := _num(p, "accel_hit_rate")) is not None]
        if was and min(was) > cfg.min_hit_rate:
            out.append(f"the baseline hit rate was {min(was):.3f}: this attempt broke the accelerated path")
    return out


def _fallback_diagnosis(points: list[dict[str, Any]], cfg: MeasurementConfig) -> list[str]:
    return [
        f"{int(fallbacks)} fallback reads at write frequency 0 (allowed {cfg.max_fallbacks_on_hit}): "
        "the reads were not all served from the state"
        for point in _read_only_points(points)
        if (fallbacks := _num(point, "accel_fallback_reads")) is not None
        and fallbacks > cfg.max_fallbacks_on_hit
    ]


def _built_diagnosis(points: list[dict[str, Any]], cfg: MeasurementConfig) -> list[str]:
    if not cfg.require_state_built:
        return []
    return [
        "no state was built (state_builds=0): reads cannot have come from an accelerator state"
        for point in _read_only_points(points)
        if _num(point, "state_builds") == 0
    ]


def _is_engine_read_axis(reported: float, ref_us: float, hit_us: float) -> bool:
    low, high = _READ_AXIS_TOLERANCE
    return low <= reported * hit_us / ref_us <= high


def _timing_diagnosis(points: list[dict[str, Any]], cfg: MeasurementConfig) -> list[str]:
    out = []
    for point in points:
        label = _label(point)
        for key in _SPEEDUP_KEYS:
            value = _num(point, key)
            if value is not None and not math.isfinite(value):
                out.append(f"{label}: {key} is {value}")
        hit_us = _num(point, "accel_hit_median_us")
        rate = _num(point, "accel_hit_rate")
        if hit_us is not None and hit_us <= 0 and (rate is None or rate > 0):
            out.append(f"{label}: accelerator hit median is {hit_us:g} us")
        ref_us = _num(point, "ref_query_median_us")
        if ref_us and not cfg.min_ref_us <= ref_us <= cfg.max_ref_s * 1e6:
            out.append(
                f"{label}: reference query median {ref_us:g} us is outside "
                f"[{cfg.min_ref_us:g} us, {cfg.max_ref_s:g} s]"
            )
        reported = _num(point, "query_only_speedup")
        if ref_us and hit_us and reported and not _is_engine_read_axis(reported, ref_us, hit_us):
            out.append(
                f"{label}: query_only_speedup {reported:g} is not the engine reference {ref_us:g} us over "
                f"the accelerator hit {hit_us:g} us: the read axis must be measured against the engine's own SQL"
            )
        fallback_us = _num(point, "fallback_median_us") or ref_us
        if hit_us and fallback_us and hit_us > _SLOWER_THAN_FALLBACK_FACTOR * fallback_us:
            out.append(
                f"{label}: accelerated read {hit_us:g} us is more than "
                f"{_SLOWER_THAN_FALLBACK_FACTOR:g}x the fallback {fallback_us:g} us"
            )
    return out


def _spread_diagnosis(scores: list[float], cfg: MeasurementConfig) -> list[str]:
    mean = statistics.fmean(scores) if scores else 0.0
    if len(scores) < 2 or mean <= 0:
        return []
    cv = statistics.pstdev(scores) / mean
    if cv < cfg.max_cv:
        return []
    return [f"sample scores vary too much (CV {cv:.2f} >= {cfg.max_cv}): {', '.join(f'{s:.3g}' for s in scores)}"]


def _stale_diagnosis(bench_binary: Path | None, query_dir: Path | None) -> list[str]:
    if bench_binary is None or query_dir is None:
        return []
    if not bench_binary.is_file():
        return [f"bench binary {bench_binary} does not exist"]
    sources = [
        p
        for p in query_dir.rglob("*")
        if p.suffix in _SOURCE_SUFFIXES and p.is_file() and not _OTHER_TARGET_DIRS.intersection(p.parts)
    ]
    newest = max(sources, key=lambda p: p.stat().st_mtime, default=None)
    if newest and newest.stat().st_mtime > bench_binary.stat().st_mtime:
        return [f"bench binary is older than {newest}: it measures a stale build"]
    return []


def _describe(point: dict[str, Any]) -> str:
    shown = {k: _num(point, k) for k in _EVIDENCE_KEYS}
    return ", ".join(f"{k}={v:g}" for k, v in shown.items() if v is not None)
