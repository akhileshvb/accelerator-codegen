"""The run's record: where it is, what happened, and where the time and money went.

The state of a run (`PipelineState`: the stage it is in, resumable), its events (`pipeline_events.jsonl` and the
stderr log lines), the experiment log the agents and the repair loop write, the timeline of wall time and cost
(`accel timeline <run dir>` -> `<run dir>/TIMELINE.md`) and the status of a run (`accel status <run dir>`).

The timeline's stages come from pipeline_events.jsonl; model and tool time from the codex rollouts (model time: from
a turn start or a tool result to the model's next item; tool time per tool: call to output); gate durations from the
pipeline log, attributed to the stage running when they were logged; cost from each session's token usage at the
model's price.
"""

from __future__ import annotations

import collections
import datetime as dt
import glob
import json
import re
import sys
import time
import urllib.request
from dataclasses import asdict, dataclass, field
from datetime import UTC, datetime
from pathlib import Path
from typing import Any

from accel_pipeline.agent import rollout_path
from accel_pipeline.config import load_workload, query_artifacts_dir

STAGES = ("analysis", "test_suite", "implementation", "optimization", "audit")


@dataclass
class PipelineState:
    query: int
    workload: str
    engine: str
    stage: str = "analysis"
    optimization_iter: int = 0
    session_id: str = ""
    # "" while running or finished; "stopped:<reason>" after a clean stop (--resume continues)
    status: str = ""
    structural_reanalysis_count: int = 0
    last_gate: dict[str, str] = field(default_factory=dict)

    @classmethod
    def load(cls, path: Path) -> PipelineState:
        data = json.loads(path.read_text(encoding="utf-8"))
        known = {f.name for f in cls.__dataclass_fields__.values()}
        filtered = {k: v for k, v in data.items() if k in known}
        return cls(**filtered)

    def save(self, path: Path) -> None:
        path.write_text(json.dumps(asdict(self), indent=2), encoding="utf-8")


def state_path(repo_root: Path, query: int, workload_name: str, engine: str) -> Path:
    workload = load_workload(workload_name, repo_root, engine=engine)
    return query_artifacts_dir(repo_root, query, workload=workload) / "pipeline_state.json"


def load_or_create(repo_root: Path, query: int, workload: str, engine: str, resume: bool) -> PipelineState:
    path = state_path(repo_root, query, workload, engine)
    if resume and path.is_file():
        return PipelineState.load(path)
    state = PipelineState(query=query, workload=workload, engine=engine, session_id=f"q{query}-{engine}")
    state.save(path)
    return state


def advance_stage(state: PipelineState, repo_root: Path) -> None:
    idx = STAGES.index(state.stage)
    if idx + 1 < len(STAGES):
        state.stage = STAGES[idx + 1]
    state.save(state_path(repo_root, state.query, state.workload, state.engine))


def events_path(repo_root: Path, query: int, workload: dict[str, Any] | None) -> Path:
    return query_artifacts_dir(repo_root, query, workload=workload) / "pipeline_events.jsonl"


def _append_event(
    repo_root: Path, query: int, workload: dict[str, Any] | None, payload: dict[str, Any]
) -> None:
    path = events_path(repo_root, query, workload)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(payload, sort_keys=True) + "\n")


def format_candidate_summary(candidate: dict[str, Any]) -> str:
    """One-line summary of a state_candidate_search entry."""
    cid = str(candidate.get("id", "?"))
    viability = str(candidate.get("viability", ""))
    family = str(candidate.get("design_family", ""))
    selected = candidate.get("selected") is True

    if viability == "rejected":
        reason = squeeze_truncate(candidate.get("rejection_reason", ""), 64)
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
        parts.append(f"grain={squeeze_truncate(str(grain), 48)}")

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
            parts.append(f"residual={squeeze_truncate(str(residual), 52)}")
    else:
        regime = candidate.get("best_regime")
        if regime:
            parts.append(squeeze_truncate(str(regime), 52))

    return " ".join(parts)


def format_candidate_detail_lines(candidate: dict[str, Any], *, index: int) -> list[str]:
    """Multi-line terminal detail for one aux-state candidate."""
    cid = str(candidate.get("id", "?"))
    viability = str(candidate.get("viability", ""))
    selected = candidate.get("selected") is True
    marker = "★ SELECTED" if selected else viability.upper() if viability else "?"
    lines = [f"[{index}] {cid}  {marker}"]

    if viability == "rejected":
        family = str(candidate.get("design_family", ""))
        reason = str(candidate.get("rejection_reason", "")).strip()
        if family:
            lines.append(f"    family: {family}")
        if reason:
            lines.append(f"    rejected because: {squeeze_truncate(reason, 120)}")
        return lines

    family = str(candidate.get("design_family", ""))
    org = str(candidate.get("physical_organization", ""))
    grain = str(candidate.get("state_grain", ""))
    if family:
        lines.append(f"    family: {family}")
    if org:
        lines.append(f"    organization: {squeeze_truncate(org, 100)}")
    if grain:
        lines.append(f"    state grain: {squeeze_truncate(grain, 90)}")

    eliminated = candidate.get("operators_eliminated")
    if isinstance(eliminated, list) and eliminated:
        elim_text = "; ".join(str(x) for x in eliminated[:4])
        if len(eliminated) > 4:
            elim_text += f"; +{len(eliminated) - 4} more"
        lines.append(f"    eliminates: {elim_text}")

    read_complexity = (
        candidate.get("asymptotic_read_complexity")
        or candidate.get("practical_read_complexity")
        or candidate.get("residual_read_algorithm")
    )
    if read_complexity:
        lines.append(f"    read path: {squeeze_truncate(str(read_complexity), 100)}")

    units = candidate.get("estimated_state_units_read")
    if isinstance(units, dict):
        typical = units.get("typical")
        if typical:
            lines.append(f"    units read (typical): {squeeze_truncate(str(typical), 80)}")

    risk = candidate.get("read_path_bottleneck_risk")
    if risk:
        lines.append(f"    bottleneck risk: {risk}")

    if not selected:
        regime = candidate.get("best_regime")
        if regime:
            lines.append(f"    best when: {squeeze_truncate(str(regime), 90)}")

    return lines


def _candidate_material_signature(candidate: dict[str, Any]) -> tuple[str, str, str]:
    return (
        str(candidate.get("design_family", "")).strip().lower(),
        str(candidate.get("physical_organization", "")).strip().lower(),
        str(candidate.get("composition_rule", "")).strip().lower(),
    )


def log_analysis_candidates(
    repo_root: Path,
    query: int,
    analysis: dict[str, Any],
    *,
    revision: bool = False,
    workload: dict[str, Any] | None = None,
) -> None:
    """Log aux-state candidate search results clearly to the terminal."""
    fitness = analysis.get("fitness_assessment") or {}
    recommended = fitness.get("recommended")
    search = analysis.get("state_candidate_search") or {}
    candidates = [c for c in (search.get("candidates") or []) if isinstance(c, dict)]

    label = "ANALYSIS REVISION" if revision else "ANALYSIS"
    print(f"[pipeline] ═══ {label}: aux-state candidates (Q{query}) ═══", file=sys.stderr)

    if not recommended:
        rationale = squeeze_truncate(str(fitness.get("rationale", "")), 160)
        print(
            f"[pipeline] fitness: recommended=false confidence={fitness.get('confidence', '-')}",
            file=sys.stderr,
        )
        if rationale:
            print(f"[pipeline]   rationale: {rationale}", file=sys.stderr)
        if not candidates:
            print("[pipeline]   (no state_candidate_search — aux-state not pursued)", file=sys.stderr)
            return

    if not candidates:
        print("[pipeline]   (no candidates in state_candidate_search)", file=sys.stderr)
        return

    selected = next((c for c in candidates if c.get("selected") is True), None)
    selected_id = str(selected.get("id", "")) if selected else ""
    viable = [c for c in candidates if c.get("viability") == "viable"]
    signatures = {_candidate_material_signature(c) for c in candidates}
    distinct_families = {sig[0] for sig in signatures if sig[0]}

    print(
        f"[pipeline] fitness: recommended={recommended} viable={len(viable)}/{len(candidates)} "
        f"distinct_families={len(distinct_families)} material_signatures={len(signatures)}",
        file=sys.stderr,
    )
    if selected_id:
        print(f"[pipeline] SELECTED → {selected_id}", file=sys.stderr)

    explanation = str(search.get("fewer_than_three_explanation") or "").strip()
    if explanation:
        print(f"[pipeline] note: {explanation}", file=sys.stderr)

    summaries = [format_candidate_summary(c) for c in candidates]
    for idx, candidate in enumerate(candidates, start=1):
        for line in format_candidate_detail_lines(candidate, index=idx):
            print(f"[pipeline] {line}", file=sys.stderr)

    if len(signatures) < min(3, len(candidates)) and not explanation:
        print(
            "[pipeline] ⚠ candidates may be too similar — need different design_family, "
            "physical_organization, or composition_rule",
            file=sys.stderr,
        )
    elif len(distinct_families) < min(3, len(viable)) and len(viable) >= 3:
        print(
            "[pipeline] ⚠ viable candidates share design families — prefer taxonomy-diverse alternatives",
            file=sys.stderr,
        )

    print(f"[pipeline] ═══ end candidate search (selected={selected_id or '-'}) ═══", file=sys.stderr)

    art = query_artifacts_dir(repo_root, query, workload=workload)
    event_path = art / "pipeline_events.jsonl"
    event_path.parent.mkdir(parents=True, exist_ok=True)
    with event_path.open("a", encoding="utf-8") as handle:
        handle.write(
            json.dumps(
                {
                    "ts": datetime.now(UTC).isoformat(),
                    "event": "analysis_candidates",
                    "revision": revision,
                    "fitness_recommended": recommended,
                    "selected_id": selected_id or None,
                    "candidate_count": len(candidates),
                    "viable_count": len(viable),
                    "distinct_families": len(distinct_families),
                    "material_signatures": len(signatures),
                    "lines": summaries,
                },
                sort_keys=True,
            )
            + "\n"
        )


def _bench_sweep_detail_lines(by_freq: dict[str, Any]) -> list[str]:
    lines: list[str] = []
    for key in sorted(by_freq.keys(), key=lambda x: int(x)):
        row = by_freq[key]
        if not isinstance(row, dict):
            continue
        freq = row.get("write_frequency_pct", key)
        wl = float(row.get("workload_speedup", 0) or 0)
        cold = float(row.get("cold_start_workload_speedup", 0) or 0)
        qo = float(row.get("query_only_speedup", 0) or 0)
        hit = float(row.get("accel_hit_rate", 0) or 0)
        writes = row.get("writes_executed", 0)
        lines.append(
            f"{freq}% writes (executed={writes}): workload={wl:.2f}x vs DuckDB, "
            f"cold_start={cold:.2f}x, query={qo:.2f}x, hit={hit * 100:.0f}%"
        )
    return lines


def _gate_context_label(change_kind: str, iteration: int) -> str:
    if change_kind == "optimization" and iteration:
        return f"{change_kind} iter={iteration}"
    if change_kind:
        return change_kind
    return "gate"


_GATE_DURATION_HINTS: dict[str, str] = {
    "aux_exit": "minutes to tens of minutes with the dataset: its phases run side by side",
    "aux_bench_sweep": "often tens of minutes: one run per write frequency",
    "aux_cost_model_calibrate": "~5-10 min when calibrating (skips if config exists)",
    "aux_cross_accel": "~1-2 min",
    "aux_multi_connection": "~1 min",
    "aux_tsan": "~1-5 min (cached TSAN build, then instrumented differential on the test subset)",
    "aux_correctness": "usually seconds; minutes with a large suite",
    "aux_correctness_iterate": "about a minute (lighter suite, fewer write probes)",
    "aux_unit": "usually seconds",
    "prims_tests": "~1 min (builds test_prims)",
}


def _gate_progress_label(index: int, count: int) -> str:
    if index > 0 and count > 0:
        return f"[{index}/{count}] "
    return ""


def log_gate_batch_start(
    query: int,
    gate_names: list[str],
    *,
    change_kind: str = "",
    iteration: int = 0,
) -> None:
    """Log the full gate list before a post-stage gate batch runs."""
    if not gate_names:
        return
    context = _gate_context_label(change_kind, iteration)
    names = ", ".join(gate_names)
    print(
        f"[pipeline] ═══ gates ({context}, Q{query}): {len(gate_names)} to run ═══",
        file=sys.stderr,
    )
    for idx, name in enumerate(gate_names, start=1):
        hint = _GATE_DURATION_HINTS.get(name)
        line = f"[pipeline]   {idx}. {name}"
        if hint:
            line += f" ({hint})"
        print(line, file=sys.stderr)


def log_gate_start(
    gate_name: str,
    query: int,
    *,
    change_kind: str = "",
    iteration: int = 0,
    index: int = 0,
    count: int = 0,
) -> None:
    """Log when an individual gate subprocess is about to start."""
    context = _gate_context_label(change_kind, iteration)
    progress = _gate_progress_label(index, count)
    hint = _GATE_DURATION_HINTS.get(gate_name)
    line = f"[pipeline] gate start: {progress}{gate_name} ({context}, Q{query})"
    if hint:
        line += f" — {hint}"
    print(line, file=sys.stderr)


def log_gate_complete(
    gate_name: str,
    *,
    status: str,
    duration_ms: int,
    change_kind: str = "",
    iteration: int = 0,
    index: int = 0,
    count: int = 0,
    detail: str = "",
    phase_timings: dict[str, float] | None = None,
) -> None:
    """Log gate subprocess completion with status, elapsed time and the per-phase seconds."""
    context = _gate_context_label(change_kind, iteration)
    progress = _gate_progress_label(index, count)
    line = (
        f"[pipeline] gate complete: {progress}{gate_name} ({context}) "
        f"status={status} duration={duration_ms}ms"
    )
    if detail:
        line += f" — {squeeze_truncate(detail, 120)}"
    print(line, file=sys.stderr)
    phases = {k: v for k, v in (phase_timings or {}).items() if k != "gate_wall"}
    if phases:
        print("[pipeline]   phases: " + " ".join(f"{k}={v:.1f}s" for k, v in phases.items()), file=sys.stderr)
    if status == "fail":
        print(f"[pipeline] gate FAILED: {gate_name}", file=sys.stderr)


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
    from accel_pipeline.optimization.bench import parse_benchmark_log

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
    workload: dict[str, Any] | None,
    *,
    gate_status: str,
    gate_data: dict[str, Any],
) -> None:
    """Print per-frequency sweep results whenever aux_bench_sweep gate runs."""
    by_freq = gate_data.get("by_write_frequency_pct") or {}
    composite = gate_data.get("composite_score", 0.0)
    wl_min = gate_data.get("composite_workload_speedup", 0.0)
    read_only = gate_data.get("read_only_query_speedup", 0.0)
    cold_start = gate_data.get("read_only_cold_start_workload_speedup", 0.0)
    notes = str(gate_data.get("notes") or gate_data.get("reason") or "").strip()

    header = (
        f"aux_bench_sweep status={gate_status} "
        f"composite={composite} wl_min={wl_min}x read_only={read_only}x cold_start={cold_start}x"
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
        workload,
        {
            "ts": datetime.now(UTC).isoformat(),
            "event": "aux_bench_sweep",
            "gate_status": gate_status,
            "composite_score": composite,
            "composite_workload_speedup": wl_min,
            "read_only_query_speedup": read_only,
            "read_only_cold_start_workload_speedup": cold_start,
            "lines": detail_lines,
            "notes": notes or None,
        },
    )


def log_event(
    repo_root: Path, query: int, workload: dict[str, Any] | None, event: str, **fields: Any
) -> None:
    payload: dict[str, Any] = {
        "ts": datetime.now(UTC).isoformat(),
        "event": event,
        **{k: v for k, v in fields.items() if v is not None},
    }
    print(_format_line(payload), file=sys.stderr)
    _append_event(repo_root, query, workload, payload)


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
    if usage.get("model_calls") is not None:
        parts.append(f"turns={usage['model_calls']}")
    if usage.get("tool_calls") is not None:
        parts.append(f"tools={usage['tool_calls']}")
    if usage.get("cost_usd") is not None:
        parts.append(f"cost=${usage['cost_usd']:.2f}")
    return " ".join(parts)


# ---------------------------------------------------------------- tokens and cost of a codex session

_PRICES_URL = "https://openrouter.ai/api/v1/models"
_PRICES_CACHE = Path.home() / ".cache" / "accel_verus" / "openrouter_prices.json"
_TOKEN_KEYS = ("input_tokens", "cached_input_tokens", "output_tokens", "reasoning_output_tokens")


def _epoch(stamp: str) -> float:
    try:
        return datetime.fromisoformat(stamp.replace("Z", "+00:00")).timestamp()
    except (ValueError, AttributeError):
        return 0.0


def session_usage(codex_home: Path, session_id: str, since: float) -> dict[str, Any]:
    """Tokens, model calls and tool calls of the part of a codex session after `since` (epoch seconds).

    The rollout carries the session's cumulative `token_count` after every model response, so a step's usage is
    the session's total at its end minus just before it started (a resumed session carries the earlier steps'
    totals). A forked session's counts start from its parent's running total, which is the parent's usage."""
    path = rollout_path(codex_home, session_id)
    if path is None:
        return {}
    before = dict.fromkeys(_TOKEN_KEYS, 0)
    after = dict.fromkeys(_TOKEN_KEYS, 0)
    model_calls = tool_calls = 0
    last_total = None
    forked = False
    with path.open(encoding="utf-8") as f:
        for line in f:
            try:
                j = json.loads(line)
            except json.JSONDecodeError:
                continue
            p = j.get("payload") or {}
            t = p.get("type")
            new = _epoch(j.get("timestamp", "")) >= since - 1.0
            if j.get("type") == "session_meta":
                forked = bool(p.get("forked_from_id"))
            elif t == "token_count":
                info = p.get("info") or {}
                total = info.get("total_token_usage")
                if not total:
                    continue
                vals = {k: int(total.get(k) or 0) for k in _TOKEN_KEYS}
                if forked and last_total is None:  # the parent's total when the fork's first response started
                    last = info.get("last_token_usage") or {}
                    before = after = {k: vals[k] - int(last.get(k) or 0) for k in _TOKEN_KEYS}
                if new:
                    if vals != last_total:  # one per model response (repeats carry no new usage)
                        model_calls += 1
                    after = vals
                else:
                    before = after = vals
                last_total = vals
            elif new and t in ("function_call", "custom_tool_call", "local_shell_call", "web_search_call"):
                tool_calls += 1
    d = {k: max(after[k] - before[k], 0) for k in _TOKEN_KEYS}
    return {"input_tokens": d["input_tokens"], "cache_read_tokens": d["cached_input_tokens"],
            "output_tokens": d["output_tokens"], "reasoning_tokens": d["reasoning_output_tokens"],
            "total_tokens": d["input_tokens"] + d["output_tokens"],
            "model_calls": model_calls, "tool_calls": tool_calls}


def _openrouter_prices() -> dict[str, dict[str, float]]:
    """USD per million tokens by model id, from OpenRouter's model list (cached for a day)."""
    try:
        if _PRICES_CACHE.is_file() and time.time() - _PRICES_CACHE.stat().st_mtime < 86400:
            return json.loads(_PRICES_CACHE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        pass
    try:
        with urllib.request.urlopen(_PRICES_URL, timeout=20) as r:
            data = json.load(r).get("data") or []
    except Exception:  # noqa: BLE001 - offline: no cost, never a failed step
        return {}
    prices = {}
    for m in data:
        pr = m.get("pricing") or {}
        try:
            prompt, completion = float(pr.get("prompt") or 0) * 1e6, float(pr.get("completion") or 0) * 1e6
            cache_read = float(pr.get("input_cache_read") or 0) * 1e6 or prompt
        except (TypeError, ValueError):
            continue
        prices[m.get("id", "")] = {"prompt": prompt, "completion": completion, "cache_read": cache_read}
    try:
        _PRICES_CACHE.parent.mkdir(parents=True, exist_ok=True)
        _PRICES_CACHE.write_text(json.dumps(prices), encoding="utf-8")
    except OSError:
        pass
    return prices


def cost_usd(model: str, usage: dict[str, Any], overrides: dict[str, Any] | None = None) -> float | None:
    """Estimated USD of a step (output includes reasoning tokens). `overrides`: {model: {prompt, completion,
    cache_read}} per million tokens, else OpenRouter's price; None when the model has no price."""
    if not usage:
        return None
    name = model.split(":", 1)[-1] if model.count(":") == 1 and "/" not in model.split(":", 1)[0] else model
    price = (overrides or {}).get(name) or (overrides or {}).get(model) or _openrouter_prices().get(name)
    if not price:
        return None
    cached = int(usage.get("cache_read_tokens") or 0)
    fresh = max(int(usage.get("input_tokens") or 0) - cached, 0)
    usd = (fresh * float(price["prompt"]) + cached * float(price.get("cache_read", price["prompt"]))
           + int(usage.get("output_tokens") or 0) * float(price["completion"])) / 1e6
    return round(usd, 4)


def step_usage(codex_home: Path, result: Any, model_id: str, started: float,
               pricing: dict[str, Any] | None = None) -> dict[str, Any] | None:
    """What an agent step used, from its codex rollout (else the backend's own count), priced."""
    sid = str(result.metadata.get("session_id") or "")
    use = session_usage(codex_home, sid, started) or usage_from_metadata(result.metadata) or {}
    cost = cost_usd(model_id, use, pricing)
    return {**use, **({"cost_usd": cost} if cost is not None else {})} or None


def usage_table(repo_root: Path, query: int, workload: dict[str, Any] | None, since: float = 0.0) -> str:
    """Per stage, the steps, time, tokens and cost of the stage_complete events after `since` (epoch seconds)."""
    path = events_path(repo_root, query, workload)
    fields = ("steps", "minutes", "input_tokens", "cache_read_tokens", "output_tokens", "model_calls", "tool_calls",
              "cost_usd")
    per: dict[str, dict[str, Any]] = {}
    for line in path.read_text(encoding="utf-8").splitlines() if path.is_file() else []:
        try:
            e = json.loads(line)
        except json.JSONDecodeError:
            continue
        if e.get("event") != "stage_complete" or _epoch(e.get("ts", "")) < since:
            continue
        s = per.setdefault(e.get("stage", "?"), dict.fromkeys(fields, 0) | {"model": e.get("model", "")})
        if s["model"] != e.get("model", ""):
            s["model"] = "(several)"
        s["steps"] += 1
        s["minutes"] += float(e.get("duration_ms") or 0) / 60000
        for k in fields[2:]:
            s[k] += float(e.get(k) or 0)
    if not per:
        return ""
    per["TOTAL"] = {k: sum(s[k] for s in per.values()) for k in fields} | {"model": ""}
    rows = [f"{'stage':<26}{'model':<30}{'steps':>6}{'min':>8}{'in Mtok':>9}{'cached':>8}{'out ktok':>9}"
            f"{'turns':>7}{'tools':>7}{'USD':>9}"]
    for name, s in per.items():
        rows.append(f"{name[:25]:<26}{str(s['model'])[:29]:<30}{int(s['steps']):>6}{s['minutes']:>8.1f}"
                    f"{s['input_tokens'] / 1e6:>9.2f}{s['cache_read_tokens'] / 1e6:>8.2f}"
                    f"{s['output_tokens'] / 1e3:>9.1f}{int(s['model_calls']):>7}{int(s['tool_calls']):>7}"
                    f"{s['cost_usd']:>9.2f}")
    return "\n".join(rows)


def log_stage_start(
    repo_root: Path,
    query: int,
    workload: dict[str, Any] | None,
    *,
    stage: str,
    model_id: str,
    optimization_iter: int = 0,
    revision: bool = False,
) -> None:
    log_event(
        repo_root,
        query,
        workload,
        "stage_start",
        stage=stage,
        model=model_id,
        optimization_iter=optimization_iter or None,
        analysis_revision=revision or None,
    )


def log_stage_complete(
    repo_root: Path,
    query: int,
    workload: dict[str, Any] | None,
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
        workload,
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
    workload: dict[str, Any] | None,
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
        workload,
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
    workload: dict[str, Any] | None,
    *,
    rerun_number: int,
    triggered_after: str,
    bottleneck_type: str,
    current_candidate_id: str,
) -> None:
    log_event(
        repo_root,
        query,
        workload,
        "structural_reanalysis_triggered",
        rerun_number=rerun_number,
        triggered_after=triggered_after,
        bottleneck_type=bottleneck_type,
        current_candidate_id=current_candidate_id,
    )


def log_structural_reanalysis_skipped(
    repo_root: Path,
    query: int,
    workload: dict[str, Any] | None,
    *,
    triggered_after: str,
    reason: str,
) -> None:
    log_event(
        repo_root,
        query,
        workload,
        "structural_reanalysis_skipped",
        triggered_after=triggered_after,
        reason=reason,
    )


def log_pipeline_summary(repo_root: Path, query: int, workload: dict[str, Any] | None) -> None:
    path = events_path(repo_root, query, workload)
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
        workload,
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
            "model_calls": payload.get("model_calls"),
            "tool_calls": payload.get("tool_calls"),
            "cost_usd": payload.get("cost_usd"),
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


def squeeze_truncate(text: str, max_chars: int) -> str:
    compact = " ".join(str(text).split())
    if len(compact) <= max_chars:
        return compact
    return compact[: max_chars - 3] + "..."


def append_experiment_log(
    repo_root: Path, query: int, line: str, *, workload: dict | None = None
) -> Path:
    path = query_artifacts_dir(repo_root, query, workload=workload) / "experiment_log.md"
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as f:
        f.write(line.rstrip() + "\n")
    return path


@dataclass
class Stage:
    name: str
    model: str
    start: dt.datetime
    end: dt.datetime | None = None


def _ts(text: str) -> dt.datetime:
    return dt.datetime.fromisoformat(text.replace("Z", "+00:00"))


def _minutes(seconds: float) -> str:
    return f"{seconds / 60:.1f}"


def read_stages(path: str) -> list[Stage]:
    stages: list[Stage] = []
    open_stage: dict[str, Stage] = {}
    for line in open(path):
        event = json.loads(line)
        if event.get("event") == "stage_start":
            open_stage[event["stage"]] = Stage(event["stage"], event.get("model", ""), _ts(event["ts"]))
            stages.append(open_stage[event["stage"]])
        elif event.get("event") == "stage_complete" and event.get("stage") in open_stage:
            open_stage.pop(event["stage"]).end = _ts(event["ts"])
    return stages


def read_rollouts(sessions: list[str]) -> tuple[list[tuple[dt.datetime, dt.datetime, str, str]], dict[str, str]]:
    """((start, end, "model" | "tool", tool name) intervals, {rollout file: model})."""
    intervals, models = [], {}
    for path in sessions:
        pending: dict[str, tuple[dt.datetime, str]] = {}
        ready = None
        for line in open(path):
            event = json.loads(line)
            when, payload = _ts(event["timestamp"]), event.get("payload") or {}
            if event.get("type") == "turn_context":
                models[path] = str(payload.get("model", ""))
            kind = payload.get("type") if event.get("type") in ("response_item", "event_msg") else None
            if kind == "task_started":
                ready = when
            elif kind in ("reasoning", "message", "function_call") and ready is not None:
                intervals.append((ready, when, "model", ""))
                ready = None
            if kind == "function_call":
                pending[payload.get("call_id")] = (when, payload.get("name", "?"))
            elif kind == "function_call_output" and payload.get("call_id") in pending:
                started, name = pending.pop(payload["call_id"])
                intervals.append((started, when, "tool", name))
                ready = when
            elif kind == "task_complete":
                ready = None
    return intervals, models


def read_gates(log_path: Path) -> dict[str, list[tuple[str, str, float]]]:
    gates: dict[str, list[tuple[str, str, float]]] = collections.defaultdict(list)
    stage = "before the stages"
    for line in log_path.read_text(errors="replace").splitlines():
        if m := re.search(r"stage start: (\w+)", line):
            stage = m.group(1)
        if m := re.search(r"gate complete: (?:\[\d+/\d+\] )?(\w+) .*status=(\w+) duration=(\d+)ms", line):
            gates[stage].append((m.group(1), m.group(2), int(m.group(3)) / 1000))
    return gates


def read_costs(sessions: list[str], models: dict[str, str]) -> list[tuple[str, str, int, float | None]]:
    """(model, session id, total tokens, USD or None when the model has no price) per session."""
    rows = []
    for path in sessions:
        session_id = Path(path).stem[-36:]
        usage = session_usage(Path(path).parents[4], session_id, 0.0)
        rows.append((models.get(path, "?"), session_id, usage.get("total_tokens", 0), cost_usd(models.get(path, ""), usage)))
    return rows


def run_files(run: Path) -> tuple[Path, list[str], list[str]]:
    """The tree of a run directory (or `run` itself when it is a tree), its event logs and its codex rollouts."""
    tree = run / "tree" if (run / "tree").is_dir() else run
    events = glob.glob(f"{tree}/pipeline/artifacts/**/pipeline_events.jsonl", recursive=True)
    sessions = sorted(glob.glob(f"{tree}/pipeline/.codex/sessions/*/*/*/*.jsonl"))
    return tree, events, sessions


def run_log(run: Path) -> Path | None:
    return next((p for p in (run / "pipeline.log", run / "logs" / "pipeline.log") if p.is_file()), None)


def render_timeline(run: Path) -> str:
    _, events, sessions = run_files(run)
    now = dt.datetime.now(dt.timezone.utc)
    stages = read_stages(events[0]) if events else []
    intervals, models = read_rollouts(sessions)
    for stage, following in zip(stages, stages[1:] + [None]):
        # a stage killed by an operator never logs completion: it ends at its last agent activity
        if stage.end is None and following is not None:
            stage.end = max((end for _, end, _, _ in intervals if stage.start <= end <= following.start), default=stage.start)
    running = {id(s) for s in stages if s.end is None}
    for stage in stages:
        stage.end = stage.end or now

    out = [f"# Timeline: {run.name} (generated {now:%H:%M}Z)", "",
           "| # | stage | model | start | wall min | model min | tool min | top tools (min, calls) |", "|---|---|---|---|---|---|---|---|"]
    wall_by_stage: collections.Counter = collections.Counter()
    for i, stage in enumerate(stages, 1):
        mine = [iv for iv in intervals if stage.start <= iv[0] < stage.end]
        wall = (stage.end - stage.start).total_seconds()
        model_s = sum((e - b).total_seconds() for b, e, kind, _ in mine if kind == "model")
        tool_s, tool_calls = collections.Counter(), collections.Counter()
        for b, e, kind, name in mine:
            if kind == "tool":
                tool_s[name] += (e - b).total_seconds()
                tool_calls[name] += 1
        top = ", ".join(f"{n} {_minutes(s)} ({tool_calls[n]})" for n, s in tool_s.most_common(4))
        out.append(f"| {i} | {stage.name}{' (running)' if id(stage) in running else ''} | {stage.model.split('/')[-1]} | "
                   f"{stage.start:%H:%M} | {_minutes(wall)} | {_minutes(model_s)} | {_minutes(sum(tool_s.values()))} | {top} |")
        wall_by_stage[stage.name] += wall
    between = sum(max(0.0, (b.start - a.end).total_seconds()) for a, b in zip(stages, stages[1:]))
    span = (stages[-1].end - stages[0].start).total_seconds() if stages else 0
    out += ["", f"Span {_minutes(span)} min; in stages {_minutes(sum(wall_by_stage.values()))}; between stages (orchestrator, "
            f"gates outside agents, operator pauses) {_minutes(between)}.", "",
            "By stage (wall min): " + ", ".join(f"{k} {_minutes(v)}" for k, v in wall_by_stage.most_common()), "", "## Gates (orchestrator-run)", ""]
    log = run_log(run)
    for stage, rows in (read_gates(log) if log else {}).items():
        out += [f"- {stage}: {name} {status} {_minutes(seconds)} min" for name, status, seconds in rows]
    costs = read_costs(sessions, models)
    priced = [usd for *_, usd in costs if usd is not None]
    out += ["", "## Cost", "", f"Total ${sum(priced):.2f}" + ("" if len(priced) == len(costs) else f" (+{len(costs) - len(priced)} unpriced sessions)"), ""]
    out += [f"- {model} {session[:8]}: {tokens:,} tokens, " + (f"${usd:.2f}" if usd is not None else "unpriced") for model, session, tokens, usd in costs]
    return "\n".join(out) + "\n"


def write_timeline(run: Path) -> str:
    text = render_timeline(run)
    (run / "TIMELINE.md").write_text(text)
    return text


LAST_EVENTS = 6


def _phase(state: PipelineState, events: list[dict]) -> str:
    if state.status:
        return state.status
    finished = any(e.get("event") == "stage_complete" and e.get("stage") == "audit" for e in events)
    return "finished" if finished else f"in {state.stage}"


def _read_events(paths: list[str]) -> list[dict]:
    return [json.loads(line) for path in paths for line in Path(path).read_text().splitlines() if line.strip()]


def render_status(run: Path) -> str:
    tree, event_paths, sessions = run_files(run)
    states = sorted(tree.glob("pipeline/artifacts/**/pipeline_state.json"), key=lambda p: p.stat().st_mtime)
    if not states:
        return f"{run}: no pipeline state yet (the run has not reached its first stage)\n"
    state = PipelineState.load(states[-1])
    events = _read_events(event_paths)
    lines = [f"run:    {run}", f"query:  {state.workload} q{state.query} on {state.engine or '-'}", f"stage:  {_phase(state, events)}"]
    if log := run_log(run):
        lines.append(f"log:    {log} (last written {(time.time() - log.stat().st_mtime) / 60:.0f} min ago)")
    _, models = read_rollouts(sessions)
    costs = read_costs(sessions, models)
    priced = [usd for *_, usd in costs if usd is not None]
    unpriced = f", {len(costs) - len(priced)} unpriced" if len(priced) < len(costs) else ""
    lines += [f"cost:   ${sum(priced):.2f} ({len(costs)} sessions{unpriced})", "last events:"]
    lines += [f"  {e.get('ts', '')[:19]}  {e.get('event', '?')}  {e.get('stage', '')}" for e in events[-LAST_EVENTS:]]
    return "\n".join(lines) + "\n"
