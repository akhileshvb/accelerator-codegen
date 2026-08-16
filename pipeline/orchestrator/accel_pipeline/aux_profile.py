"""Aux-state profile digests and custom instrumentation capture.

Base-kernel optimization writes ``profile_summary.json`` via ``ACCEL_BENCH_PROFILE``.
Aux workloads expose store diagnostics + hit latency from quick/fair sweeps. This
module synthesizes useful profile artifacts for MCP tools and optimization_runtime.
"""

from __future__ import annotations

import json
import re
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from accel_pipeline.optimization_scoreboard import (
    aggregate_custom_profile_events,
    build_scoreboard,
    custom_profiling_contract,
    profile_freshness,
    write_scoreboard,
)
from accel_pipeline.optimization_signals import (
    extract_store_diagnostics,
    organize_diagnostics_evidence,
)

CUSTOM_PROFILE_PREFIX = "ACCEL_CUSTOM_PROFILE"
_CUSTOM_LINE_RE = re.compile(
    rf"{re.escape(CUSTOM_PROFILE_PREFIX)}\s*[:=]?\s*(\{{.*\}}|\S.+)\s*$"
)


def parse_custom_profile_events(output: str) -> list[dict[str, Any]]:
    """Parse agent-emitted ``ACCEL_CUSTOM_PROFILE {...}`` lines from bench output."""
    events: list[dict[str, Any]] = []
    for line in (output or "").splitlines():
        match = _CUSTOM_LINE_RE.search(line.strip())
        if not match:
            continue
        raw = match.group(1).strip()
        try:
            payload = json.loads(raw)
            if isinstance(payload, dict):
                events.append(payload)
            else:
                events.append({"value": payload})
        except json.JSONDecodeError:
            events.append({"raw": raw})
    return events


def _read_json(path: Path) -> dict[str, Any]:
    if not path.is_file():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}


def _metrics_from_best_valid(best_valid: dict[str, Any]) -> dict[str, Any]:
    metrics = dict(best_valid.get("metrics") or {})
    if best_valid.get("score") is not None and "optimization_score" not in metrics:
        metrics["optimization_score"] = best_valid["score"]
    return metrics


def _pick_metrics(art: Path) -> tuple[dict[str, Any], str]:
    """Prefer freshest measured aux metrics available on disk."""
    profile = _read_json(art / "profile_summary.json")
    if profile.get("workload_kind") == "aux_state" and (
        profile.get("store_diagnostics") or profile.get("read_only_query_speedup")
    ):
        return profile, "profile_summary.json"

    best_valid = _read_json(art / "best_valid_baseline.json")
    if best_valid:
        return _metrics_from_best_valid(best_valid), "best_valid_baseline.json"

    aux_summary = _read_json(art / "aux_bench_summary.json")
    if aux_summary:
        return aux_summary, "aux_bench_summary.json"

    opt_state = _read_json(art / "optimization_state.json")
    best_summary = opt_state.get("best_summary") or {}
    if best_summary:
        return dict(best_summary), "optimization_state.json"

    return {}, ""


def build_aux_profile_summary(
    metrics: dict[str, Any],
    *,
    source: str,
    custom_events: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """Shape aux bench metrics into a profile_summary-compatible digest."""
    diag = extract_store_diagnostics(metrics)
    read_only = float(
        metrics.get("read_only_query_speedup")
        or metrics.get("optimization_score")
        or metrics.get("speedup_median")
        or 0.0
    )
    hit_us = float(diag.get("accel_hit_median_us") or metrics.get("accel_hit_median_us") or 0.0)
    hit_rate = float(diag.get("accel_hit_rate") or metrics.get("accel_hit_rate") or 0.0)
    validity_us = float(diag.get("validity_scan_us_median") or 0.0)
    parts_read = float(diag.get("partitions_read_per_query_median") or 0.0)
    state_bytes = float(diag.get("state_bytes_read_median") or 0.0)

    custom_agg = aggregate_custom_profile_events(list(custom_events or []))
    phases: dict[str, float] = {}
    if hit_us > 0:
        phases["accel_hit_us"] = hit_us
    if validity_us > 0:
        phases["validity_scan_us"] = validity_us
    for phase_row in custom_agg.get("phases") or []:
        name = str(phase_row.get("phase") or "")
        mean_us = phase_row.get("mean_us")
        if name and mean_us is not None:
            phases[name] = float(mean_us)

    dominant_phase = str(custom_agg.get("dominant_phase") or "")
    dominant_us = float(custom_agg.get("dominant_phase_mean_us") or 0.0)
    if not dominant_phase and phases:
        # Prefer non-hit aggregate phases when present.
        candidates = {k: v for k, v in phases.items() if k != "accel_hit_us"}
        if candidates:
            dominant_phase, dominant_us = max(candidates.items(), key=lambda kv: kv[1])
        else:
            dominant_phase, dominant_us = max(phases.items(), key=lambda kv: kv[1])

    timed_sum = float(custom_agg.get("timed_phases_mean_sum_us") or 0.0)
    residual = None
    if hit_us > 0 and timed_sum > 0:
        residual = round(hit_us - timed_sum, 3)

    signals: list[str] = []
    if read_only > 0:
        signals.append(f"read_only_query_speedup={read_only:.3f}x")
    if hit_us > 0:
        signals.append(f"accel_hit_median_us={hit_us:.1f}")
    if hit_rate > 0:
        signals.append(f"accel_hit_rate={hit_rate:.3f}")
    if parts_read > 0:
        signals.append(f"partitions_read_per_query_median={parts_read:.1f}")
    if state_bytes > 0:
        signals.append(f"state_bytes_read_median={state_bytes:.0f}")
    if dominant_phase:
        signals.append(f"dominant_custom_phase={dominant_phase}@{dominant_us:.1f}us")
    if custom_agg.get("work_counters_mean"):
        for key, value in list(custom_agg["work_counters_mean"].items())[:4]:
            signals.append(f"{key}={value}")
    if custom_events:
        signals.append(f"custom_profile_events={len(custom_events)}")
    if residual is not None:
        signals.append(f"residual_vs_hit_us={residual}")

    return {
        "workload_kind": "aux_state",
        "source": source,
        "measured_at": datetime.now(timezone.utc).isoformat(),
        "score_metric": "read_only_query_speedup",
        "read_only_query_speedup": read_only,
        "composite_score": float(metrics.get("composite_score") or 0.0),
        "accel_hit_median_us": hit_us,
        "accel_hit_rate": hit_rate,
        "setup_full_build_ms": float(metrics.get("setup_full_build_ms") or 0.0),
        "store_diagnostics": diag,
        "diagnostics_evidence": organize_diagnostics_evidence(diag),
        "profile_phases_us": phases,
        "phase_breakdown": custom_agg.get("phases") or [],
        "work_counters_mean": custom_agg.get("work_counters_mean") or {},
        "residual_vs_hit_us": residual,
        "dominant_phase": dominant_phase,
        "dominant_phase_us": dominant_us,
        "profile_signals": signals,
        "custom_profile_events": list(custom_events or [])[-40:],
        "custom_profile_aggregate": custom_agg,
        "counters": {
            "valid_partitions": diag.get("valid_partitions"),
            "total_slots": diag.get("total_slots"),
            "store_execute_calls": diag.get("store_execute_calls"),
            "partitions_read_per_query_median": diag.get("partitions_read_per_query_median"),
            "state_bytes_read_median": diag.get("state_bytes_read_median"),
            **(custom_agg.get("work_counters_mean") or {}),
        },
    }


def write_aux_profile_artifacts(
    art: Path,
    metrics: dict[str, Any],
    *,
    source: str,
    bench_output: str = "",
    repo_root: Path | None = None,
    editable_rel_paths: list[str] | None = None,
) -> dict[str, Any]:
    """Write profile_summary.json (+ optional custom_profile.json) for aux digests."""
    art.mkdir(parents=True, exist_ok=True)
    custom_events = parse_custom_profile_events(bench_output)
    summary = build_aux_profile_summary(metrics, source=source, custom_events=custom_events)
    (art / "profile_summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    if custom_events:
        (art / "custom_profile.json").write_text(
            json.dumps(
                {
                    "events": custom_events,
                    "aggregate": summary.get("custom_profile_aggregate") or {},
                    "measured_at": summary.get("measured_at"),
                    "source": source,
                },
                indent=2,
            ),
            encoding="utf-8",
        )
    baseline = {
        "workload_kind": "aux_state",
        "speedup_median": summary.get("read_only_query_speedup"),
        "read_only_query_speedup": summary.get("read_only_query_speedup"),
        "accelerator_median_us": summary.get("accel_hit_median_us"),
        "store_diagnostics": summary.get("store_diagnostics") or {},
        "source": source,
        "measured_at": summary.get("measured_at"),
    }
    (art / "baseline_summary.json").write_text(json.dumps(baseline, indent=2), encoding="utf-8")
    board = build_scoreboard(art, is_aux=True, last_metrics=metrics)
    write_scoreboard(art, board)
    freshness = profile_freshness(
        art, repo_root=repo_root, editable_rel_paths=editable_rel_paths
    )
    summary["scoreboard"] = board
    summary["freshness"] = freshness
    return summary


def build_aux_profile_digest(
    art: Path,
    *,
    repo_root: Path | None = None,
    editable_rel_paths: list[str] | None = None,
) -> dict[str, Any]:
    """Compact MCP digest for aux-state optimization."""
    metrics, source = _pick_metrics(art)
    custom = _read_json(art / "custom_profile.json")
    custom_events = list(custom.get("events") or [])
    if not custom_events:
        existing = _read_json(art / "profile_summary.json")
        custom_events = list(existing.get("custom_profile_events") or [])

    if metrics:
        summary = build_aux_profile_summary(
            metrics, source=source or "artifacts", custom_events=custom_events
        )
    else:
        summary = {
            "workload_kind": "aux_state",
            "source": "missing",
            "profile_signals": [
                "no aux metrics on disk — call run_profile or run_quick_benchmark"
            ],
            "store_diagnostics": {},
            "custom_profile_events": custom_events,
            "phase_breakdown": [],
        }

    fair = _read_json(art / "aux_bench_summary.json")
    opt_state = _read_json(art / "optimization_state.json")
    best_valid = _read_json(art / "best_valid_baseline.json")
    kernel_manifest = _read_json(art / "best_kernel_snapshot" / "manifest.json")
    board = build_scoreboard(art, is_aux=True, last_metrics=metrics or None)
    write_scoreboard(art, board)
    freshness = profile_freshness(
        art, repo_root=repo_root, editable_rel_paths=editable_rel_paths
    )

    return {
        "workload_kind": "aux_state",
        "scoreboard": board,
        "freshness": freshness,
        "profile_summary": summary,
        "phase_breakdown": summary.get("phase_breakdown") or [],
        "work_counters_mean": summary.get("work_counters_mean") or {},
        "residual_vs_hit_us": summary.get("residual_vs_hit_us"),
        "baseline_summary": _read_json(art / "baseline_summary.json"),
        "store_diagnostics": summary.get("store_diagnostics") or {},
        "fair_sweep": {
            "read_only_query_speedup": fair.get("read_only_query_speedup"),
            "composite_score": fair.get("composite_score"),
            "accel_hit_median_us": (fair.get("by_write_frequency_pct") or {})
            .get("0", {})
            .get("accel_hit_median_us"),
        }
        if fair
        else {},
        "optimization_state_best": {
            "best_composite_score": opt_state.get("best_composite_score"),
            "read_only_query_speedup": (opt_state.get("best_summary") or {}).get(
                "read_only_query_speedup"
            ),
        }
        if opt_state
        else {},
        "best_valid_baseline": {
            "score": best_valid.get("score"),
            "attempt_id": best_valid.get("attempt_id"),
            "accel_hit_median_us": (best_valid.get("metrics") or {}).get("accel_hit_median_us"),
        }
        if best_valid
        else {},
        "best_kernel_snapshot": {
            "speedup_median": kernel_manifest.get("speedup_median"),
            "change_kind": kernel_manifest.get("change_kind"),
        }
        if kernel_manifest
        else {},
        "custom_profiling": custom_profiling_contract(is_aux=True),
        "measurement_rules": [
            "Call run_profile on the CURRENT tree before forming a hypothesis.",
            "If freshness.stale is true, do not hypothesize from this digest — remeasure.",
            "Cite dominant_phase, phase_breakdown means, and work_counters_mean by name/value.",
            "A/B keep/regress vs scoreboard.best_valid_score only.",
            "After accept/restore, profile is stale until run_profile runs again.",
        ],
    }


def load_aux_store_diagnostics(art: Path) -> tuple[dict[str, Any], str]:
    """Find store diagnostics across aux artifact names."""
    for name in (
        "profile_summary.json",
        "best_valid_baseline.json",
        "aux_bench_summary.json",
        "optimization_state.json",
        "baseline_summary.json",
        "bench_summary.json",
    ):
        data = _read_json(art / name)
        if name == "best_valid_baseline.json":
            data = _metrics_from_best_valid(data)
        elif name == "optimization_state.json":
            data = dict(data.get("best_summary") or {})
        diag = extract_store_diagnostics(data)
        if not diag and isinstance(data.get("store_diagnostics"), dict):
            diag = extract_store_diagnostics(data["store_diagnostics"])
        if diag:
            return diag, name
    return {}, ""
