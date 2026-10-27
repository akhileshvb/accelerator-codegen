"""Parse base (non-aux) benchmark gate output and track optimization progress."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from accel_pipeline.config import load_pipeline_config
from accel_pipeline.workload import resolve_workload_paths, load_query_analysis, query_artifacts_dir
from accel_pipeline.optimization.kernel_hot_path_review import analyze_kernel_source, format_kernel_review_for_prompt
from accel_pipeline.optimization.kernel_snapshot import (
    has_best_kernel_snapshot,
    maybe_restore_best_kernel,
    snapshot_best_kernel,
)
from accel_pipeline.runlog import append_experiment_log
from accel_pipeline.optimization.profile_investigation import build_profile_investigation


def _parse_csv_row(header: list[str], values: list[str]) -> dict[str, Any]:
    row: dict[str, Any] = {}
    for key, raw in zip(header, values):
        key = key.strip()
        if not key:
            continue
        try:
            if "." in raw:
                row[key] = float(raw)
            else:
                row[key] = int(raw)
        except ValueError:
            row[key] = raw.strip()
    return row


def _normalize_benchmark_row(row: dict[str, Any]) -> dict[str, Any]:
    if "speedup_median" not in row and "speedup" in row:
        row["speedup_median"] = row["speedup"]
    if "reference_median_us" not in row and "reference_ms" in row:
        row["reference_median_us"] = float(row["reference_ms"]) * 1000.0
    if "accelerator_median_us" not in row and "accelerator_ms" in row:
        row["accelerator_median_us"] = float(row["accelerator_ms"]) * 1000.0
    if "reference_median_us" not in row and "reference_median_ms" in row:
        row["reference_median_us"] = float(row["reference_median_ms"]) * 1000.0
    if "accelerator_median_us" not in row and "accelerator_median_ms" in row:
        row["accelerator_median_us"] = float(row["accelerator_median_ms"]) * 1000.0
    return row


def parse_benchmark_log(log_tail: str) -> dict[str, Any]:
    """Parse benchmark CSV output (baseline or profile gate log_tail)."""
    lines = [line.strip() for line in log_tail.strip().splitlines() if line.strip()]
    header: list[str] | None = None
    data_row: dict[str, Any] | None = None
    summary_row: dict[str, Any] | None = None
    profile_row: dict[str, Any] | None = None

    for line in lines:
        if line.startswith("label,"):
            header = [part.strip() for part in line.split(",")]
            continue
        if header is None:
            continue
        row = _normalize_benchmark_row(_parse_csv_row(header, line.split(",")))
        label = str(row.get("label", ""))
        if label.startswith("summary_"):
            summary_row = row
        elif "profile" in label.lower() or row.get("pre_scan_us") is not None:
            profile_row = row
        elif data_row is None:
            data_row = row

    if profile_row:
        return profile_row
    result = summary_row or data_row or {}
    return _normalize_benchmark_row(result)


def _read_gate_log_tail(art: Path, gate_name: str) -> str:
    gate_path = art / "gates" / f"{gate_name}.json"
    if not gate_path.is_file():
        return ""
    data = json.loads(gate_path.read_text(encoding="utf-8"))
    return str(data.get("log_tail", "") or "")


def write_baseline_summary(art: Path) -> dict[str, Any]:
    parsed = parse_benchmark_log(_read_gate_log_tail(art, "baseline"))
    summary = {
        "source": "baseline_gate",
        "reference_median_us": parsed.get("reference_median_us"),
        "accelerator_median_us": parsed.get("accelerator_median_us"),
        "reference_median_ms": parsed.get("reference_ms") or (
            parsed.get("reference_median_us", 0) / 1000.0 if parsed.get("reference_median_us") else None
        ),
        "accelerator_median_ms": parsed.get("accelerator_ms") or (
            parsed.get("accelerator_median_us", 0) / 1000.0 if parsed.get("accelerator_median_us") else None
        ),
        "speedup_median": parsed.get("speedup_median"),
        "speedup_mean": parsed.get("speedup_mean"),
        "raw": parsed,
    }
    (art / "baseline_summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    return summary


def write_profile_summary(art: Path) -> dict[str, Any]:
    profile_path = art / "profile_summary.json"
    log_tail = ""
    if profile_path.is_file():
        data = json.loads(profile_path.read_text(encoding="utf-8"))
        log_tail = str(data.get("log_tail", "") or "")
    if not log_tail:
        log_tail = _read_gate_log_tail(art, "profile")

    parsed = parse_benchmark_log(log_tail)
    raw_phases = {
        "pre_scan_us": parsed.get("pre_scan_us"),
        "pre_next_us": parsed.get("pre_next_us"),
        "pre_consume_us": parsed.get("pre_consume_us"),
        "finalize_us": parsed.get("finalize_us"),
        "sub_scan_us": parsed.get("sub_scan_us"),
        "num_scan_us": parsed.get("num_scan_us"),
        "num_aggregate_us": parsed.get("num_aggregate_us"),
    }
    summary = {
        "source": "profile_gate",
        "speedup_median": parsed.get("speedup_median"),
        "phases_us": {k: v for k, v in raw_phases.items() if v is not None},
        "counters": {
            "qualifying_rows": parsed.get("qualifying_rows"),
            "output_groups": parsed.get("output_groups") or parsed.get("active_groups"),
            "pre_chunks": parsed.get("pre_chunks"),
            "pre_rows": parsed.get("pre_rows") or parsed.get("pre_rows_scanned"),
            "generic_rows": parsed.get("generic_rows") or parsed.get("generic_fallback_rows"),
            "flat_chunks": parsed.get("flat_chunks") or parsed.get("pre_flat"),
            "dict_chunks": parsed.get("dict_chunks") or parsed.get("pre_dict"),
            "generic_chunks": parsed.get("generic_chunks") or parsed.get("pre_generic"),
            "distinct_inserts": parsed.get("distinct_inserts"),
        },
        "raw": parsed,
    }
    (art / "profile_summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    return summary


def build_optimization_diagnosis(
    baseline: dict[str, Any],
    profile: dict[str, Any],
    *,
    best_speedup_median: float = 0.0,
    analysis: dict[str, Any] | None = None,
    kernel_review: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Turn gate measurements into actionable feedback for the next optimization iteration."""
    speedup = float(baseline.get("speedup_median") or 0.0)
    ref_us = baseline.get("reference_median_us")
    acc_us = baseline.get("accelerator_median_us")
    ref_ms = baseline.get("reference_median_ms")
    acc_ms = baseline.get("accelerator_median_ms")
    if ref_ms is None and ref_us is not None:
        ref_ms = float(ref_us) / 1000.0
    if acc_ms is None and acc_us is not None:
        acc_ms = float(acc_us) / 1000.0

    findings: list[str] = []
    lacking: list[str] = []

    if best_speedup_median > 0 and speedup + 0.01 < best_speedup_median:
        lacking.insert(
            0,
            f"REGRESSION: current {speedup:.3f}x vs best {best_speedup_median:.3f}x — "
            "the pipeline auto-restores the best kernel snapshot before the next iteration.",
        )

    if speedup < 1.0:
        lacking.append(
            f"Accelerator is slower than DuckDB: speedup_median={speedup:.3f}x "
            f"(ref≈{ref_ms:.1f}ms vs accel≈{acc_ms:.1f}ms)."
            if ref_ms and acc_ms
            else f"Accelerator is slower than DuckDB: speedup_median={speedup:.3f}x."
        )
    elif best_speedup_median > 0 and speedup + 0.01 < best_speedup_median:
        lacking.append(
            f"Slower than best snapshot: speedup_median={speedup:.3f}x vs best {best_speedup_median:.3f}x."
        )
    else:
        findings.append(f"Beats DuckDB: speedup_median={speedup:.3f}x.")
        if best_speedup_median > 0 and speedup <= best_speedup_median + 0.01:
            lacking.append(
                f"At or below best snapshot ({best_speedup_median:.3f}x) — ship a new bundle to improve further."
            )
        else:
            findings.append("New best vs DuckDB — keep optimizing while improvements remain.")

    phases = {k: float(v) for k, v in (profile.get("phases_us") or {}).items() if v is not None}
    raw_profile = profile.get("raw") or {}
    for key in ("sub_scan_us", "num_scan_us", "num_aggregate_us"):
        if key not in phases and raw_profile.get(key) is not None:
            phases[key] = float(raw_profile[key])
    counters = profile.get("counters") or {}
    dominant_phase: str | None = None
    dominant_us = 0.0
    if phases:
        dominant_phase, dominant_us = max(phases.items(), key=lambda item: item[1])
        total_phase_us = sum(phases.values()) or 1.0
        pct = 100.0 * dominant_us / total_phase_us
        findings.append(f"Dominant phase: {dominant_phase} = {dominant_us:.0f} us ({pct:.0f}% of profiled scan path).")

    generic_rows = int(counters.get("generic_rows") or 0)
    pre_rows = int(counters.get("pre_rows") or 0)
    flat_chunks = int(counters.get("flat_chunks") or 0)
    dict_chunks = int(counters.get("dict_chunks") or 0)
    generic_chunks = int(counters.get("generic_chunks") or 0)
    distinct_inserts = int(counters.get("distinct_inserts") or 0)
    qualifying_rows = int(counters.get("qualifying_rows") or 0)
    total_chunks = flat_chunks + dict_chunks + generic_chunks

    if generic_rows > 0 and pre_rows > 0:
        pct = 100.0 * generic_rows / pre_rows
        findings.append(f"generic_fallback_rows={generic_rows} ({pct:.1f}% of {pre_rows} scanned rows).")
        lacking.append(
            f"Almost all rows use the slow generic vector path ({pct:.0f}%); "
            "DICTIONARY/FLAT fast paths are not active."
        )
    elif generic_rows > 1_000_000:
        findings.append(f"generic_fallback_rows={generic_rows} (pre_rows counter missing in profile CSV).")
        lacking.append("Generic vector fallback dominates; fast paths are not wired.")

    if total_chunks > 0 and generic_chunks > 0:
        pct = 100.0 * generic_chunks / total_chunks
        findings.append(
            f"generic_fallback_chunks={generic_chunks} ({pct:.1f}% of {total_chunks} chunks)."
        )

    if pre_rows > 0 and distinct_inserts > 0:
        ratio = distinct_inserts / pre_rows
        findings.append(f"distinct_inserts={distinct_inserts} ({ratio:.1%} of {pre_rows} rows).")

    if pre_rows > 0 and qualifying_rows > 0:
        amplification = pre_rows / qualifying_rows
        if amplification >= 5:
            findings.append(
                f"scan_amplification={amplification:.1f}x (pre_rows={pre_rows:,}, qualifying_rows={qualifying_rows:,})."
            )

    num_scan_us = float(phases.get("num_scan_us") or 0)
    num_aggregate_us = float(phases.get("num_aggregate_us") or 0)
    pre_consume_us = float(phases.get("pre_consume_us") or 0)
    aggregate_us = num_aggregate_us or pre_consume_us
    if num_scan_us > 0 and aggregate_us > 0:
        agg_share = aggregate_us / max(num_scan_us, 1.0)
        findings.append(
            f"aggregate_us={aggregate_us:.0f} ({agg_share:.0%} of num_scan_us={num_scan_us:.0f})."
        )
    elif num_scan_us > 0 and aggregate_us == 0 and qualifying_rows > 10_000:
        findings.append(
            "Fused scan: num_scan_us likely includes per-row aggregation (aggregate_us not reported separately)."
        )

    kernel_smells = list((kernel_review or {}).get("smells") or [])
    if kernel_smells:
        high = [s for s in kernel_smells if s.get("severity") == "high"]
        if high:
            findings.append(f"static_kernel_review: {len(high)} high-severity smell(s) on file.")

    if not phases and speedup < 1.0:
        lacking.append("No profile phase breakdown available — profile gate may not have run or counters are zero.")

    investigation = build_profile_investigation(
        phases=phases,
        counters=counters,
        speedup=speedup,
        best_speedup=best_speedup_median,
        dominant_phase=dominant_phase,
        kernel_smells=kernel_smells,
    )

    strategy_palette = list(investigation.get("strategy_palette") or [])
    suggestions: list[str] = []
    if analysis:
        simplest = (
            (analysis.get("logical_plan") or {}).get("simplest_execution_strategy")
            or (analysis.get("query_summary") or {}).get("pattern")
        )
        if simplest:
            suggestions.append(f"analysis_plan: {simplest}")
        for op in (analysis.get("logical_plan") or {}).get("operators") or []:
            if isinstance(op, str) and op.strip():
                suggestions.append(f"analysis_operator: {op.strip()}")
                break

    for item in reversed(suggestions):
        if item not in strategy_palette:
            strategy_palette.insert(0, item)

    suggested_bundle = strategy_palette[:8]
    suggested_focus: list[str] = []
    if lacking:
        suggested_focus.append(lacking[0])
    if strategy_palette:
        suggested_focus.append(f"Try coordinated bundle from palette: {strategy_palette[0]}")
    if dominant_phase:
        suggested_focus.append(f"Dominant phase `{dominant_phase}` — verify the inner loop in the code before editing")

    return {
        "speedup_median": speedup,
        "best_speedup_median": best_speedup_median,
        "beats_duckdb": speedup >= 1.0,
        "regressed_vs_best": best_speedup_median > 0 and speedup + 0.01 < best_speedup_median,
        "reference_median_ms": ref_ms,
        "accelerator_median_ms": acc_ms,
        "dominant_phase": dominant_phase,
        "dominant_phase_us": dominant_us,
        "findings": findings,
        "lacking": lacking,
        "profile_signals": investigation.get("profile_signals") or [],
        "investigation_questions": investigation.get("investigation_questions") or [],
        "strategy_palette": strategy_palette,
        "suggested_bundle": suggested_bundle,
        "suggested_focus": suggested_focus[:4],
        "profile_counters": counters,
        "profile_phases_us": phases,
        "kernel_review": kernel_review or {},
    }


def format_optimization_feedback(diagnosis: dict[str, Any]) -> str:
    lines = [
        "## Optimization gate feedback (read before making changes)",
        "",
        f"- speedup_median: **{diagnosis.get('speedup_median', 0):.3f}x** (goal: beat DuckDB and improve `best_speedup_median`)",
    ]
    best = diagnosis.get("best_speedup_median")
    if best is not None and float(best) > 0:
        lines.append(f"- best_speedup_median: **{float(best):.3f}x** (do not ship regressions below this)")
    if diagnosis.get("regressed_vs_best"):
        lines.append("- **REGRESSION** vs best — pipeline will auto-restore the best kernel snapshot before the next iteration.")
    if diagnosis.get("reference_median_ms") is not None:
        lines.append(
            f"- latency: DuckDB≈{diagnosis['reference_median_ms']:.1f}ms, "
            f"accelerator≈{diagnosis['accelerator_median_ms']:.1f}ms"
        )
    if diagnosis.get("dominant_phase"):
        lines.append(
            f"- dominant phase: `{diagnosis['dominant_phase']}` "
            f"({diagnosis.get('dominant_phase_us', 0):.0f} us)"
        )
    if diagnosis.get("lacking"):
        lines.append("")
        lines.append("### What's lacking")
        for item in diagnosis["lacking"][:3]:
            lines.append(f"- {item}")
    if diagnosis.get("findings"):
        lines.append("")
        lines.append("### Profile findings")
        for item in diagnosis["findings"][:4]:
            lines.append(f"- {item}")
    if diagnosis.get("investigation_questions"):
        lines.append("")
        lines.append("### Questions to investigate")
        for item in diagnosis["investigation_questions"][:5]:
            lines.append(f"- {item}")
    palette = diagnosis.get("strategy_palette") or []
    if palette:
        lines.append("")
        lines.append("### Strategy palette (pick a coordinated bundle)")
        for i, item in enumerate(palette[:6], 1):
            lines.append(f"{i}. {item}")
    bundle = diagnosis.get("suggested_bundle") or []
    if bundle:
        lines.append("")
        lines.append("### Suggested bundle (this iteration)")
        for item in bundle[:4]:
            lines.append(f"- {item}")
    kernel_review = diagnosis.get("kernel_review") or {}
    kernel_lines = format_kernel_review_for_prompt(kernel_review)
    if kernel_lines and not kernel_lines[0].startswith("- No high-confidence"):
        lines.append("")
        lines.append("### Kernel hot-path review (static)")
        for item in kernel_lines:
            lines.append(item)
    lines.append("")
    lines.append(
        "Form a hot-path hypothesis from profile signals, pick techniques from the palette, "
        "and A/B test a coordinated bundle — not isolated one-liners."
    )
    return "\n".join(lines)


def write_optimization_feedback(
    art: Path,
    baseline: dict[str, Any],
    profile: dict[str, Any],
    *,
    best_speedup_median: float = 0.0,
    analysis: dict[str, Any] | None = None,
    iteration: int = 0,
    kernel_review: dict[str, Any] | None = None,
) -> dict[str, Any]:
    diagnosis = build_optimization_diagnosis(
        baseline,
        profile,
        best_speedup_median=best_speedup_median,
        analysis=analysis,
        kernel_review=kernel_review,
    )
    diagnosis["after_iteration"] = iteration
    (art / "optimization_feedback.json").write_text(json.dumps(diagnosis, indent=2), encoding="utf-8")
    md = format_optimization_feedback(diagnosis)
    (art / "optimization_feedback.md").write_text(md, encoding="utf-8")
    return diagnosis


def load_base_optimization_state(art: Path) -> dict[str, Any]:
    path = art / "optimization_state.json"
    if not path.is_file():
        return {"best_speedup_median": 0.0, "iterations": []}
    return json.loads(path.read_text(encoding="utf-8"))


def save_base_optimization_state(art: Path, state: dict[str, Any]) -> None:
    (art / "optimization_state.json").write_text(json.dumps(state, indent=2), encoding="utf-8")


def compact_summary_for_prompt(summary: dict[str, Any]) -> str:
    """Drop nested raw/log blobs for optimization prompts."""
    if not summary:
        return "{}"
    compact = {k: v for k, v in summary.items() if k != "raw"}
    return json.dumps(compact, separators=(",", ":"), ensure_ascii=False)


def process_base_bench_after_gates(
    repo_root: Path,
    query: int,
    workload: dict[str, Any],
    *,
    iteration: int = 0,
    change_kind: str = "implementation",
) -> dict[str, Any]:
    """Update baseline/profile summaries, optimization state, and inter-iter feedback after gates."""
    art = query_artifacts_dir(repo_root, query, workload=workload)
    baseline = write_baseline_summary(art)
    profile = write_profile_summary(art)

    analysis_path, analysis = load_query_analysis(repo_root, query, workload=workload)
    if not analysis:
        analysis = None

    speedup = float(baseline.get("speedup_median") or 0.0)
    state = load_base_optimization_state(art)
    prev_best = float(state.get("best_speedup_median") or 0.0)

    paths = resolve_workload_paths(load_pipeline_config(repo_root=repo_root), workload, query)
    kernel_review = analyze_kernel_source(repo_root / paths["kernel_path"])

    diagnosis = write_optimization_feedback(
        art,
        baseline,
        profile,
        best_speedup_median=prev_best,
        analysis=analysis,
        iteration=iteration,
        kernel_review=kernel_review,
    )

    improved = speedup > prev_best

    entry = {
        "iteration": iteration,
        "change_kind": change_kind,
        "speedup_median": speedup,
        "reference_median_us": baseline.get("reference_median_us"),
        "accelerator_median_us": baseline.get("accelerator_median_us"),
        "improved": improved,
        "dominant_phase": diagnosis.get("dominant_phase"),
    }
    state.setdefault("iterations", []).append(entry)
    if improved:
        state["best_speedup_median"] = speedup
    state["last_speedup_median"] = speedup
    save_base_optimization_state(art, state)

    pipeline_config = load_pipeline_config(repo_root=repo_root)
    best_now = float(state.get("best_speedup_median") or 0.0)

    should_snapshot = improved or (
        not has_best_kernel_snapshot(art) and speedup >= 1.0 and change_kind in ("implementation", "optimization")
    )
    if should_snapshot:
        snapshot_best_kernel(
            pipeline_config,
            workload,
            query,
            speedup_median=speedup,
            iteration=iteration,
            change_kind=change_kind,
        )
    elif best_now > 0 and speedup + 0.005 < best_now:
        maybe_restore_best_kernel(
            pipeline_config,
            workload,
            query,
            best_speedup_median=best_now,
            last_speedup_median=speedup,
            reason=f"after {change_kind} iter {iteration}",
        )

    ref_us = baseline.get("reference_median_us")
    acc_us = baseline.get("accelerator_median_us")
    append_experiment_log(
        repo_root,
        query,
        f"- {change_kind} iter {iteration}: speedup_median={speedup:.3f}x "
        f"(ref={ref_us}us accel={acc_us}us) "
        f"phase={diagnosis.get('dominant_phase') or 'n/a'} "
        f"{'IMPROVED' if improved else 'no improvement'}",
        workload=workload,
    )

    from accel_pipeline.runlog import log_base_bench_summary

    log_base_bench_summary(
        repo_root,
        query,
        diagnosis,
        change_kind=change_kind,
        iteration=iteration,
        improved=improved,
        workload=workload,
    )

    return {
        "speedup_median": speedup,
        "improved": improved,
        "prev_best": prev_best,
        "baseline": baseline,
        "profile": profile,
        "diagnosis": diagnosis,
    }
