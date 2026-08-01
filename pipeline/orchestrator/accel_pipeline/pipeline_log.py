"""Orchestrator logging to stderr and pipeline_events.jsonl."""

from __future__ import annotations

import json
import sys
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from accel_pipeline.paths import query_artifacts_dir


def events_path(repo_root: Path, query: int) -> Path:
    return query_artifacts_dir(repo_root, query) / "pipeline_events.jsonl"


def _append_event(repo_root: Path, query: int, payload: dict[str, Any]) -> None:
    path = events_path(repo_root, query)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(payload, sort_keys=True) + "\n")


def _truncate_text(text: str, limit: int) -> str:
    compact = " ".join(str(text).split())
    if len(compact) <= limit:
        return compact
    return compact[: limit - 3] + "..."


def format_candidate_summary(candidate: dict[str, Any]) -> str:
    """One-line summary of a state_candidate_search entry."""
    cid = str(candidate.get("id", "?"))
    viability = str(candidate.get("viability", ""))
    family = str(candidate.get("design_family", ""))
    selected = candidate.get("selected") is True

    if viability == "rejected":
        reason = _truncate_text(candidate.get("rejection_reason", ""), 64)
        base = f"{cid} rejected {family}"
        return f"{base}: {reason}" if reason else base

    parts = [cid]
    if selected:
        parts.append("[SELECTED]")
    if viability:
        parts.append(viability)
    if family:
        parts.append(family)

    grain = candidate.get("state_grain")
    if grain:
        parts.append(f"grain={_truncate_text(str(grain), 48)}")

    risk = candidate.get("read_path_bottleneck_risk")
    if risk:
        parts.append(f"risk={risk}")

    if selected:
        residual = (
            candidate.get("asymptotic_read_complexity")
            or candidate.get("residual_read_algorithm")
            or candidate.get("read_path_scaling_variable")
        )
        if residual:
            parts.append(f"residual={_truncate_text(str(residual), 52)}")
    else:
        regime = candidate.get("best_regime")
        if regime:
            parts.append(_truncate_text(str(regime), 52))

    return " ".join(parts)


def log_analysis_candidates(
    repo_root: Path,
    query: int,
    analysis: dict[str, Any],
    *,
    revision: bool = False,
) -> None:
    """Log short per-candidate lines after analysis.json is written."""
    search = analysis.get("state_candidate_search") or {}
    candidates = [c for c in (search.get("candidates") or []) if isinstance(c, dict)]
    if not candidates:
        return

    fitness = analysis.get("fitness_assessment") or {}
    selected = next((c for c in candidates if c.get("selected") is True), None)
    selected_id = str(selected.get("id", "")) if selected else ""
    summaries = [format_candidate_summary(c) for c in candidates]

    label = "analysis revision candidates" if revision else "analysis candidates"
    header = (
        f"{label}: n={len(candidates)} fitness_recommended={fitness.get('recommended')} "
        f"selected={selected_id or '-'}"
    )
    print(f"[pipeline] {header}", file=sys.stderr)
    for summary in summaries:
        prefix = "* " if summary.startswith(selected_id) and selected_id else "- "
        if "[SELECTED]" in summary:
            prefix = "* "
        print(f"[pipeline]   {prefix}{summary}", file=sys.stderr)

    _append_event(
        repo_root,
        query,
        {
            "ts": datetime.now(UTC).isoformat(),
            "event": "analysis_candidates",
            "revision": revision,
            "fitness_recommended": fitness.get("recommended"),
            "selected_id": selected_id or None,
            "candidate_count": len(candidates),
            "lines": summaries,
        },
    )


def _bench_sweep_detail_lines(by_freq: dict[str, Any]) -> list[str]:
    lines: list[str] = []
    for key in sorted(by_freq.keys(), key=lambda x: int(x)):
        row = by_freq[key]
        if not isinstance(row, dict):
            continue
        freq = row.get("write_frequency_pct", key)
        wl = float(row.get("workload_speedup", 0) or 0)
        qo = float(row.get("query_only_speedup", 0) or 0)
        hit = float(row.get("accel_hit_rate", 0) or 0)
        writes = row.get("writes_executed", 0)
        lines.append(
            f"{freq}% writes (executed={writes}): workload={wl:.2f}x vs DuckDB, "
            f"query={qo:.2f}x, hit={hit * 100:.0f}%"
        )
    return lines


def _gate_context_label(change_kind: str, iteration: int) -> str:
    if change_kind == "optimization" and iteration:
        return f"{change_kind} iter={iteration}"
    if change_kind:
        return change_kind
    return "gate"


def log_base_bench_gate(
    repo_root: Path,
    query: int,
    *,
    gate_name: str,
    gate_status: str,
    gate_data: dict[str, Any],
    change_kind: str = "",
    iteration: int = 0,
    workload: dict[str, Any] | None = None,
) -> None:
    """Log correctness / baseline speedup / profile phase breakdown after each gate."""
    from accel_pipeline.base_bench import parse_benchmark_log

    label = _gate_context_label(change_kind, iteration)
    lines: list[str] = []

    if gate_name == "correctness":
        lines.append(f"gate correctness ({label}): status={gate_status}")
    elif gate_name == "baseline":
        parsed = parse_benchmark_log(str(gate_data.get("log_tail", "")))
        speedup = float(parsed.get("speedup_median") or 0.0)
        ref_ms = parsed.get("reference_ms")
        acc_ms = parsed.get("accelerator_ms")
        if ref_ms is None and parsed.get("reference_median_us") is not None:
            ref_ms = float(parsed["reference_median_us"]) / 1000.0
        if acc_ms is None and parsed.get("accelerator_median_us") is not None:
            acc_ms = float(parsed["accelerator_median_us"]) / 1000.0
        line = f"gate baseline ({label}): status={gate_status} speedup_median={speedup:.3f}x"
        if ref_ms is not None and acc_ms is not None:
            line += f" ref={ref_ms:.1f}ms accel={acc_ms:.1f}ms"
        if speedup < 1.0:
            line += " (SLOWER than DuckDB)"
        elif speedup >= 2.0:
            line += " (target met)"
        lines.append(line)
    elif gate_name == "profile":
        log_tail = str(gate_data.get("log_tail", ""))
        if not log_tail.strip():
            summary_path = gate_data.get("profile_summary")
            if summary_path:
                path = Path(str(summary_path))
                if path.is_file():
                    summary = json.loads(path.read_text(encoding="utf-8"))
                    log_tail = str(summary.get("log_tail", ""))
        parsed = parse_benchmark_log(log_tail)
        phases = {
            k: float(v)
            for k, v in {
                "pre_scan_us": parsed.get("pre_scan_us"),
                "pre_next_us": parsed.get("pre_next_us"),
                "pre_consume_us": parsed.get("pre_consume_us"),
                "finalize_us": parsed.get("finalize_us"),
            }.items()
            if v is not None
        }
        line = f"gate profile ({label}): status={gate_status}"
        if phases:
            dominant = max(phases.items(), key=lambda item: item[1])
            line += f" dominant={dominant[0]}={dominant[1]:.0f}us"
            parts = [f"{k}={v:.0f}us" for k, v in sorted(phases.items(), key=lambda x: -x[1])]
            lines.append(line)
            lines.append("  phases: " + ", ".join(parts))
        else:
            reason = str(gate_data.get("reason") or "no phase data in profile output")
            lines.append(f"{line} ({reason})")
        generic = parsed.get("generic_rows") or parsed.get("generic_fallback_rows")
        if generic is not None:
            lines.append(f"  counters: generic_rows={generic} pre_rows={parsed.get('pre_rows', 'n/a')}")
    else:
        return

    for line in lines:
        print(f"[pipeline] {line}", file=sys.stderr)

    art = query_artifacts_dir(repo_root, query, workload=workload)
    event_path = art / "pipeline_events.jsonl"
    event_path.parent.mkdir(parents=True, exist_ok=True)
    with event_path.open("a", encoding="utf-8") as handle:
        handle.write(
            json.dumps(
                {
                    "ts": datetime.now(UTC).isoformat(),
                    "event": f"gate_{gate_name}",
                    "gate_status": gate_status,
                    "change_kind": change_kind or None,
                    "iteration": iteration or None,
                    "lines": lines,
                },
                sort_keys=True,
            )
            + "\n"
        )


def log_base_bench_summary(
    repo_root: Path,
    query: int,
    diagnosis: dict[str, Any],
    *,
    change_kind: str = "",
    iteration: int = 0,
    improved: bool = False,
    workload: dict[str, Any] | None = None,
) -> None:
    """Roll-up after baseline+profile gates: speedup, gap, and suggested focus."""
    label = _gate_context_label(change_kind, iteration)
    speedup = float(diagnosis.get("speedup_median") or 0.0)
    best = float(diagnosis.get("best_speedup_median") or 0.0)
    header = (
        f"bench summary ({label}): speedup_median={speedup:.3f}x "
        f"best={best:.3f}x beats_duckdb={diagnosis.get('beats_duckdb')} "
        f"{'IMPROVED' if improved else 'no improvement'}"
    )
    print(f"[pipeline] {header}", file=sys.stderr)
    if diagnosis.get("dominant_phase"):
        print(
            f"[pipeline]   dominant phase: {diagnosis['dominant_phase']} "
            f"({diagnosis.get('dominant_phase_us', 0):.0f} us)",
            file=sys.stderr,
        )
    for item in diagnosis.get("lacking", [])[:2]:
        print(f"[pipeline]   lacking: {item}", file=sys.stderr)
    for item in diagnosis.get("suggested_focus", [])[:2]:
        print(f"[pipeline]   next focus: {item}", file=sys.stderr)

    art = query_artifacts_dir(repo_root, query, workload=workload)
    event_path = art / "pipeline_events.jsonl"
    with event_path.open("a", encoding="utf-8") as handle:
        handle.write(
            json.dumps(
                {
                    "ts": datetime.now(UTC).isoformat(),
                    "event": "bench_summary",
                    "change_kind": change_kind or None,
                    "iteration": iteration or None,
                    "speedup_median": speedup,
                    "best_speedup_median": best,
                    "improved": improved,
                    "dominant_phase": diagnosis.get("dominant_phase"),
                },
                sort_keys=True,
            )
            + "\n"
        )


def log_bench_sweep_results(
    repo_root: Path,
    query: int,
    *,
    gate_status: str,
    gate_data: dict[str, Any],
) -> None:
    """Print per-frequency sweep results whenever aux_bench_sweep gate runs."""
    by_freq = gate_data.get("by_write_frequency_pct") or {}
    composite = gate_data.get("composite_score", 0.0)
    wl_min = gate_data.get("composite_workload_speedup", 0.0)
    read_only = gate_data.get("read_only_query_speedup", 0.0)
    notes = str(gate_data.get("notes") or gate_data.get("reason") or "").strip()

    header = (
        f"aux_bench_sweep status={gate_status} "
        f"composite={composite} wl_min={wl_min}x read_only={read_only}x"
    )
    if notes:
        header += f" ({notes})"

    detail_lines = _bench_sweep_detail_lines(by_freq)
    if not detail_lines:
        detail_lines = [notes or "no sweep rows parsed"]

    print(f"[pipeline] {header}", file=sys.stderr)
    for line in detail_lines:
        print(f"[pipeline]   {line}", file=sys.stderr)

    _append_event(
        repo_root,
        query,
        {
            "ts": datetime.now(UTC).isoformat(),
            "event": "aux_bench_sweep",
            "gate_status": gate_status,
            "composite_score": composite,
            "composite_workload_speedup": wl_min,
            "read_only_query_speedup": read_only,
            "lines": detail_lines,
            "notes": notes or None,
        },
    )


def log_event(repo_root: Path, query: int, event: str, **fields: Any) -> None:
    payload: dict[str, Any] = {
        "ts": datetime.now(UTC).isoformat(),
        "event": event,
        **{k: v for k, v in fields.items() if v is not None},
    }
    print(_format_line(payload), file=sys.stderr)
    _append_event(repo_root, query, payload)


def usage_from_metadata(metadata: dict[str, Any] | None) -> dict[str, int] | None:
    if not metadata:
        return None
    usage = metadata.get("usage")
    if not isinstance(usage, dict) or not usage.get("total_tokens"):
        return None
    return {
        "input_tokens": int(usage.get("input_tokens", 0)),
        "output_tokens": int(usage.get("output_tokens", 0)),
        "total_tokens": int(usage.get("total_tokens", 0)),
        "cache_read_tokens": int(usage.get("cache_read_tokens", 0)),
        "cache_write_tokens": int(usage.get("cache_write_tokens", 0)),
    }


def format_usage(usage: dict[str, int] | None) -> str:
    if not usage:
        return "tokens=n/a"
    parts = [
        f"in={usage['input_tokens']}",
        f"out={usage['output_tokens']}",
        f"total={usage['total_tokens']}",
    ]
    cache_read = usage.get("cache_read_tokens", 0)
    cache_write = usage.get("cache_write_tokens", 0)
    if cache_read or cache_write:
        parts.append(f"cache_r={cache_read}")
        parts.append(f"cache_w={cache_write}")
    return " ".join(parts)


def log_stage_start(
    repo_root: Path,
    query: int,
    *,
    stage: str,
    model_id: str,
    optimization_iter: int = 0,
    revision: bool = False,
) -> None:
    log_event(
        repo_root,
        query,
        "stage_start",
        stage=stage,
        model=model_id,
        optimization_iter=optimization_iter or None,
        analysis_revision=revision or None,
    )


def log_stage_complete(
    repo_root: Path,
    query: int,
    *,
    stage: str,
    model_id: str,
    backend: str,
    duration_ms: int | None,
    usage: dict[str, int] | None,
    optimization_iter: int = 0,
    run_id: str = "",
    dry_run: bool = False,
    prompt_chars: int | None = None,
) -> None:
    log_event(
        repo_root,
        query,
        "stage_complete",
        stage=stage,
        model=model_id,
        backend=backend,
        optimization_iter=optimization_iter or None,
        duration_ms=duration_ms,
        run_id=run_id or None,
        dry_run=dry_run or None,
        prompt_chars=prompt_chars,
        **(usage or {}),
    )


def log_structural_reanalysis_request(
    repo_root: Path,
    query: int,
    *,
    status: str,
    source_stage: str,
    source_iteration: int,
    bottleneck_type: str = "",
    current_candidate_id: str = "",
    reason: str = "",
) -> None:
    log_event(
        repo_root,
        query,
        "structural_reanalysis_request",
        status=status,
        source_stage=source_stage,
        source_iteration=source_iteration,
        bottleneck_type=bottleneck_type or None,
        current_candidate_id=current_candidate_id or None,
        reason=reason or None,
    )


def log_structural_reanalysis_triggered(
    repo_root: Path,
    query: int,
    *,
    rerun_number: int,
    triggered_after: str,
    bottleneck_type: str,
    current_candidate_id: str,
) -> None:
    log_event(
        repo_root,
        query,
        "structural_reanalysis_triggered",
        rerun_number=rerun_number,
        triggered_after=triggered_after,
        bottleneck_type=bottleneck_type,
        current_candidate_id=current_candidate_id,
    )


def log_structural_reanalysis_skipped(
    repo_root: Path,
    query: int,
    *,
    triggered_after: str,
    reason: str,
) -> None:
    log_event(
        repo_root,
        query,
        "structural_reanalysis_skipped",
        triggered_after=triggered_after,
        reason=reason,
    )


def log_pipeline_summary(repo_root: Path, query: int) -> None:
    path = events_path(repo_root, query)
    if not path.is_file():
        return

    totals = {
        "input_tokens": 0,
        "output_tokens": 0,
        "total_tokens": 0,
    }
    stage_completes = 0
    structural_requests = 0
    structural_triggered = 0

    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        name = event.get("event")
        if name == "stage_complete":
            stage_completes += 1
            for key in totals:
                totals[key] += int(event.get(key, 0) or 0)
        elif name == "structural_reanalysis_request" and event.get("status") == "queued":
            structural_requests += 1
        elif name == "structural_reanalysis_triggered":
            structural_triggered += 1

    log_event(
        repo_root,
        query,
        "pipeline_summary",
        stages_completed=stage_completes,
        structural_requests_queued=structural_requests,
        structural_reruns=structural_triggered,
        **totals,
    )


def _format_line(payload: dict[str, Any]) -> str:
    event = payload.get("event", "event")
    if event == "stage_start":
        suffix = ""
        if payload.get("analysis_revision"):
            suffix = " [analysis revision]"
        iter_part = ""
        if payload.get("optimization_iter"):
            iter_part = f" iter={payload['optimization_iter']}"
        return (
            f"[pipeline] stage start: {payload.get('stage')}{iter_part} "
            f"model={payload.get('model')}{suffix}"
        )
    if event == "stage_complete":
        usage = {
            "input_tokens": payload.get("input_tokens"),
            "output_tokens": payload.get("output_tokens"),
            "total_tokens": payload.get("total_tokens"),
            "cache_read_tokens": payload.get("cache_read_tokens", 0),
            "cache_write_tokens": payload.get("cache_write_tokens", 0),
        }
        if not usage["total_tokens"]:
            usage = None
        iter_part = ""
        if payload.get("optimization_iter"):
            iter_part = f" iter={payload['optimization_iter']}"
        if payload.get("dry_run"):
            return (
                f"[pipeline] stage complete (dry-run): {payload.get('stage')}{iter_part} "
                f"prompt_chars={payload.get('prompt_chars', 0)}"
            )
        duration = payload.get("duration_ms")
        duration_part = f" duration={duration}ms" if duration is not None else ""
        return (
            f"[pipeline] stage complete: {payload.get('stage')}{iter_part} "
            f"{format_usage(usage)}{duration_part} backend={payload.get('backend')}"
        )
    if event == "structural_reanalysis_request":
        return (
            f"[pipeline] structural reanalysis {payload.get('status')}: "
            f"stage={payload.get('source_stage')} "
            f"bottleneck={payload.get('bottleneck_type') or '-'} "
            f"candidate={payload.get('current_candidate_id') or '-'}"
            + (f" reason={payload.get('reason')}" if payload.get("reason") else "")
        )
    if event == "structural_reanalysis_triggered":
        return (
            f"[pipeline] structural reanalysis triggered: "
            f"rerun={payload.get('rerun_number')} after={payload.get('triggered_after')} "
            f"bottleneck={payload.get('bottleneck_type')} "
            f"candidate={payload.get('current_candidate_id')}"
        )
    if event == "structural_reanalysis_skipped":
        return (
            f"[pipeline] structural reanalysis skipped after {payload.get('triggered_after')}: "
            f"{payload.get('reason')}"
        )
    if event == "structural_reanalysis_resume":
        return (
            f"[pipeline] resuming at {payload.get('resume_stage')} "
            f"(structural rerun {payload.get('rerun_number')} after {payload.get('triggered_after')})"
        )
    if event == "pipeline_summary":
        return (
            f"[pipeline] summary: stages={payload.get('stages_completed')} "
            f"tokens in={payload.get('input_tokens')} out={payload.get('output_tokens')} "
            f"total={payload.get('total_tokens')} "
            f"structural_requests={payload.get('structural_requests_queued')} "
            f"structural_reruns={payload.get('structural_reruns')}"
        )
    parts = [f"{k}={v}" for k, v in payload.items() if k not in {"ts", "event"}]
    return f"[pipeline] {event}: " + " ".join(parts)
