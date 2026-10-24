"""Stable optimization scoreboard + custom-profile aggregation (query-agnostic)."""

from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from accel_pipeline.jsonio import read_json

SCOREBOARD_FILE = "optimization_scoreboard.json"

# Suggested phase names for agent instrumentation (not enforced).
RECOMMENDED_PHASES: tuple[str, ...] = (
    "setup",
    "load",
    "scan",
    "filter",
    "aggregate",
    "count",
    "sort",
    "materialize",
    "emit",
    "finalize",
)

# Work-volume keys commonly useful across queries.
WORK_COUNT_KEYS: tuple[str, ...] = (
    "rows",
    "emitted_rows",
    "scanned_rows",
    "groups",
    "blocks",
    "partitions",
    "keys",
    "items",
    "bytes",
    "candidates",
    "hits",
    "misses",
)


def _as_float(value: Any) -> float | None:
    if value is None or value == "":
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def primary_score_from_metrics(
    metrics: dict[str, Any] | None,
    *,
    is_aux: bool,
    workload: dict[str, Any] | None = None,
) -> float:
    if not metrics:
        return 0.0
    if is_aux:
        # Prefer dual-axis formula (read + write-mix). Fall back through
        # aux_optimization_score which handles missing write-mix / hit-time.
        from accel_pipeline.optimization_eval import aux_optimization_score

        scored = aux_optimization_score(metrics, workload)
        if scored > 0:
            return scored
        for key in (
            "optimization_score",
            "read_only_query_speedup",
            "speedup_median",
            "composite_query_speedup",
        ):
            val = _as_float(metrics.get(key))
            if val is not None and val > 0:
                return val
        return 0.0
    for key in ("speedup_median", "optimization_score"):
        val = _as_float(metrics.get(key))
        if val is not None and val > 0:
            return val
    return 0.0


def latency_from_metrics(metrics: dict[str, Any] | None, *, is_aux: bool) -> float | None:
    if not metrics:
        return None
    if is_aux:
        for key in ("accel_hit_median_us", "accelerator_median_us"):
            val = _as_float(metrics.get(key))
            if val is not None and val > 0:
                return val
        return None
    for key in ("accelerator_median_us", "accel_hit_median_us"):
        val = _as_float(metrics.get(key))
        if val is not None and val > 0:
            return val
    return None


def read_speedup_vs_engine(metrics: dict[str, Any] | None) -> float | None:
    """Engine's own SQL median over the accelerator hit median, on the read-only (0% write) row."""
    if not metrics:
        return None
    rows = dict(metrics.get("by_write_frequency_pct") or {})
    row = dict(rows.get("0") or next(iter(rows.values()), {}))
    ref_us = _as_float(row.get("ref_query_median_us"))
    hit_us = _as_float(row.get("accel_hit_median_us")) or _as_float(metrics.get("accel_hit_median_us"))
    if ref_us and hit_us and ref_us > 0 and hit_us > 0:
        return round(ref_us / hit_us, 3)
    return None


def read_axes(metrics: dict[str, Any] | None) -> dict[str, float | None]:
    """The read axis on its own, against both references (the write axis never masks it)."""
    base_kernel = _as_float((metrics or {}).get("read_only_query_speedup"))
    return {
        "vs_base_kernel": base_kernel if base_kernel and base_kernel > 0 else None,
        "vs_engine": read_speedup_vs_engine(metrics),
    }


def aggregate_custom_profile_events(events: list[dict[str, Any]]) -> dict[str, Any]:
    """Aggregate ACCEL_CUSTOM_PROFILE events into phase timers + work counters.

    Accepted event shapes (generic):
      {"phase":"sort","us":12.3,"emitted_rows":180}
      {"phase":"aggregate","aggregate_us":40.0,"groups":12}
      {"sort_us":12.3,"rows":100}   # phase inferred from *_us key
    """
    phase_samples: dict[str, list[float]] = {}
    phase_counts: dict[str, dict[str, list[float]]] = {}
    global_counts: dict[str, list[float]] = {}

    for event in events:
        if not isinstance(event, dict):
            continue
        phase = str(event.get("phase") or event.get("name") or "").strip()
        us = _as_float(event.get("us"))
        if us is None:
            # Infer from first *_us key.
            for key, value in event.items():
                if str(key).endswith("_us"):
                    us = _as_float(value)
                    if not phase:
                        phase = str(key)[: -len("_us")]
                    break
        if not phase:
            phase = "unnamed"
        if us is not None and us >= 0:
            phase_samples.setdefault(phase, []).append(us)

        for key in WORK_COUNT_KEYS:
            if key not in event:
                continue
            val = _as_float(event[key])
            if val is None:
                continue
            phase_counts.setdefault(phase, {}).setdefault(key, []).append(val)
            global_counts.setdefault(key, []).append(val)

    phases_out: list[dict[str, Any]] = []
    for phase, samples in phase_samples.items():
        samples_sorted = sorted(samples)
        mid = samples_sorted[len(samples_sorted) // 2]
        entry: dict[str, Any] = {
            "phase": phase,
            "n": len(samples),
            "mean_us": round(sum(samples) / len(samples), 3),
            "p50_us": round(mid, 3),
            "total_us": round(sum(samples), 3),
            "max_us": round(max(samples), 3),
        }
        counts = phase_counts.get(phase) or {}
        if counts:
            entry["counts_mean"] = {
                k: round(sum(vs) / len(vs), 3) for k, vs in counts.items() if vs
            }
        phases_out.append(entry)

    phases_out.sort(key=lambda row: float(row.get("mean_us") or 0.0), reverse=True)
    work_means = {
        k: round(sum(vs) / len(vs), 3) for k, vs in global_counts.items() if vs
    }
    dominant = phases_out[0] if phases_out else {}
    timed_mean_sum = sum(float(p.get("mean_us") or 0.0) for p in phases_out)

    return {
        "event_count": len(events),
        "phases": phases_out,
        "dominant_phase": dominant.get("phase") or "",
        "dominant_phase_mean_us": dominant.get("mean_us"),
        "timed_phases_mean_sum_us": round(timed_mean_sum, 3),
        "work_counters_mean": work_means,
        "recommended_phases": list(RECOMMENDED_PHASES),
        "recommended_count_keys": list(WORK_COUNT_KEYS),
    }


def editable_source_mtime(
    repo_root: Path,
    rel_paths: list[str],
) -> float:
    newest = 0.0
    for rel in rel_paths:
        path = repo_root / rel
        if path.is_file():
            newest = max(newest, path.stat().st_mtime)
    return newest


def profile_freshness(
    art: Path,
    *,
    repo_root: Path | None = None,
    editable_rel_paths: list[str] | None = None,
) -> dict[str, Any]:
    """Detect stale profile artifacts vs editable sources / best_valid."""
    profile_path = art / "profile_summary.json"
    custom_path = art / "custom_profile.json"
    best_path = art / "best_valid_baseline.json"
    now = datetime.now(timezone.utc).isoformat()

    profile_mtime = profile_path.stat().st_mtime if profile_path.is_file() else 0.0
    custom_mtime = custom_path.stat().st_mtime if custom_path.is_file() else 0.0
    best_mtime = best_path.stat().st_mtime if best_path.is_file() else 0.0
    source_mtime = 0.0
    if repo_root is not None and editable_rel_paths:
        source_mtime = editable_source_mtime(repo_root, editable_rel_paths)

    reasons: list[str] = []
    stale = False
    if not profile_path.is_file():
        stale = True
        reasons.append("missing profile_summary.json — call run_profile")
    if source_mtime > 0 and profile_mtime > 0 and source_mtime > profile_mtime + 0.01:
        stale = True
        reasons.append(
            "editable sources newer than profile_summary.json — re-run run_profile "
            "on the CURRENT tree before hypothesizing"
        )
    if best_mtime > 0 and profile_mtime > 0 and best_mtime > profile_mtime + 0.01:
        stale = True
        reasons.append(
            "best_valid_baseline newer than profile_summary.json — profile is from "
            "an older candidate; re-run run_profile after restore/accept"
        )
    if profile_path.is_file() and not custom_path.is_file():
        reasons.append(
            "no custom_profile.json — add split timers/counts (phase+us+rows/groups) "
            "gated on ACCEL_AUX_CUSTOM_PROFILE / ACCEL_BENCH_PROFILE"
        )
    elif custom_mtime > 0 and source_mtime > custom_mtime + 0.01:
        stale = True
        reasons.append("custom_profile.json older than sources — remeasure")

    return {
        "checked_at": now,
        "stale": stale,
        "reasons": reasons,
        "profile_summary_mtime": profile_mtime or None,
        "custom_profile_mtime": custom_mtime or None,
        "best_valid_mtime": best_mtime or None,
        "editable_sources_mtime": source_mtime or None,
    }


def build_scoreboard(
    art: Path,
    *,
    is_aux: bool,
    last_metrics: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Authoritative compare-against board for A/B and digests."""
    best_valid = read_json(art / "best_valid_baseline.json")
    fair = read_json(art / "aux_bench_summary.json") if is_aux else {}
    profile = read_json(art / "profile_summary.json")
    baseline = read_json(art / "baseline_summary.json")
    opt_state = read_json(art / "optimization_state.json")

    best_metrics = dict(best_valid.get("metrics") or {})
    if best_valid.get("score") is not None:
        best_metrics.setdefault("optimization_score", best_valid["score"])

    measured = dict(last_metrics or profile or baseline or {})
    # Always recompute from metrics so dual-axis weight changes apply to stored
    # best_valid baselines (do not trust a stale absolute score alone).
    best_score = primary_score_from_metrics(best_metrics, is_aux=is_aux)
    if best_score <= 0 and best_valid.get("score") is not None:
        scored = _as_float(best_valid.get("score"))
        if scored is not None and scored > 0:
            best_score = scored
    if best_score <= 0 and is_aux:
        best_score = primary_score_from_metrics(
            dict(opt_state.get("best_summary") or {}), is_aux=True
        )
    if best_score <= 0 and not is_aux:
        best_score = primary_score_from_metrics(
            {"speedup_median": opt_state.get("best_speedup_median")}, is_aux=False
        )

    # Avoid dual-vs-read unit mismatch: baseline/profile often omit write_mix while
    # best_valid is dual-axis. Inherit best write-mix for scoring only so
    # improvement_pct compares like units (otherwise digests show bogus +100%+).
    if is_aux and measured and best_metrics:
        from accel_pipeline.optimization_eval import _write_mix_from_metrics

        if _write_mix_from_metrics(measured) <= 0 and _write_mix_from_metrics(best_metrics) > 0:
            measured["write_mix_workload_speedup"] = best_metrics.get(
                "write_mix_workload_speedup"
            )
            if best_metrics.get("write_mix_pct") is not None:
                measured.setdefault("write_mix_pct", best_metrics.get("write_mix_pct"))

    measured_score = primary_score_from_metrics(measured, is_aux=is_aux)
    fair_score = (
        primary_score_from_metrics(fair, is_aux=True) if is_aux and fair else 0.0
    )
    score_metric = (
        "dual_axis_weighted(read^0.75 * write_mix^0.25)"
        if is_aux
        else "speedup_median"
    )
    delta = None
    if best_score > 0 and measured_score > 0:
        delta = round(100.0 * (measured_score - best_score) / best_score, 4)

    board = {
        "score_metric": score_metric,
        "compare_against": "best_valid_score",
        "best_valid_score": best_score or None,
        "best_valid_attempt_id": best_valid.get("attempt_id"),
        "best_valid_hit_us": latency_from_metrics(best_metrics, is_aux=is_aux),
        "last_measured_score": measured_score or None,
        "last_measured_hit_us": latency_from_metrics(measured, is_aux=is_aux),
        "improvement_pct_vs_best_valid": delta,
        "read_speedup": (
            {"best_valid": read_axes(best_metrics), "last_measured": read_axes(measured)} if is_aux else None
        ),
        "fair_sweep_score": fair_score or None,
        "guidance": (
            "A/B keep/regress and yield decisions must use best_valid_score "
            f"({score_metric}). Ignore ephemeral local baselines that disagree "
            "with best_valid. After accept/restore, call run_profile before new hyps."
        ),
    }
    return board


def write_scoreboard(art: Path, board: dict[str, Any]) -> Path:
    art.mkdir(parents=True, exist_ok=True)
    path = art / SCOREBOARD_FILE
    payload = {
        **board,
        "updated_at": datetime.now(timezone.utc).isoformat(),
    }
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    return path


def custom_profiling_contract(*, is_aux: bool) -> dict[str, Any]:
    env = "ACCEL_AUX_CUSTOM_PROFILE" if is_aux else "ACCEL_BENCH_PROFILE"
    return {
        "env_gate": env,
        "line_prefix": "ACCEL_CUSTOM_PROFILE",
        "required_fields": ["phase", "us"],
        "recommended_phases": list(RECOMMENDED_PHASES),
        "recommended_count_keys": list(WORK_COUNT_KEYS),
        "example": (
            'ACCEL_CUSTOM_PROFILE {"phase":"sort","us":120.5,'
            '"emitted_rows":180,"groups":180}'
        ),
        "rules": [
            f"Gate all instrumentation on getenv(\"{env}\") — no-op when unset.",
            "Emit one JSON object per timed region (and optional per-query totals).",
            "Always include work counts (rows/groups/items/bytes) with timers.",
            "Re-run run_profile after every accept/restore before a new hypothesis.",
            "Hypothesis must cite dominant_phase + counts from a non-stale digest.",
        ],
    }
