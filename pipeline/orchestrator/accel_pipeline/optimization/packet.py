"""Compact optimization context packet with hard size budget."""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any

from accel_pipeline.digests import (
    analysis_for_optimization,
    compact_base_analysis_for_optimization,
)
from accel_pipeline.optimization.base_bench import compact_summary_for_prompt
from accel_pipeline.config import (
    PipelineConfig,
    resolve_optimization_edit_paths,
    resolve_workload_paths,
    load_query_analysis,
    query_artifacts_dir,
)
from accel_pipeline.optimization.kernel_hot_path_review import analyze_kernel_source
from accel_pipeline.optimization.signals import (
    diagnostic_deltas,
    extract_store_diagnostics,
    format_store_diagnostics_markdown,
    organize_diagnostics_evidence,
)
from accel_pipeline.optimization.transformations import format_precedents_markdown, retrieve_transformations

CONTEXT_MAX_CHARS = 80_000
CONTEXT_MAX_LINES = 1200

_SECTION_LINE_CAPS = {
    "summary": 120,
    "hot_path": 600,
    "signatures": 80,
    "measurements": 100,
    "evidence": 100,
    "failed_hypotheses": 40,
}


def _truncate_lines(text: str, max_lines: int, *, label: str = "") -> str:
    lines = text.splitlines()
    if len(lines) <= max_lines:
        return text
    kept = lines[:max_lines]
    suffix = f"\n... [truncated {len(lines) - max_lines} lines from {label}]" if label else ""
    return "\n".join(kept) + suffix


def _truncate_chars(text: str, max_chars: int) -> str:
    if len(text) <= max_chars:
        return text
    return text[: max_chars - 40] + f"\n... [truncated at {max_chars} chars]"


def _extract_signatures(path: Path, max_lines: int = 50) -> str:
    if not path.is_file():
        return ""
    lines: list[str] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = line.strip()
        if stripped.startswith("//") or stripped.startswith("/*") or stripped.startswith("*"):
            if lines and lines[-1].startswith("//"):
                lines.append(line)
            continue
        if re.match(r"^(class|struct|enum|using|namespace|#)", stripped):
            lines.append(line)
            continue
        if re.match(r"^(inline\s+)?(static\s+)?[\w:<>,\s*&]+\s+\w+\s*\([^;]*\)\s*(const)?\s*;", stripped):
            lines.append(line)
            continue
        if re.match(r"^(inline\s+)?(static\s+)?[\w:<>,\s*&]+\s+\w+\s*\(", stripped) and "{" not in stripped:
            lines.append(line)
            continue
        if stripped == "};" or stripped == "}":
            lines.append(line)
    return _truncate_lines("\n".join(lines[:max_lines * 2]), max_lines, label=str(path.name))


def _hot_path_excerpt(kernel_path: Path, kernel_review: dict[str, Any], max_lines: int) -> str:
    if not kernel_path.is_file():
        return ""
    text = kernel_path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    hot_line_nums: set[int] = set()
    for smell in kernel_review.get("smells") or []:
        line = smell.get("line")
        if isinstance(line, int) and line > 0:
            for offset in range(-5, 15):
                hot_line_nums.add(line + offset)
    if not hot_line_nums:
        return _truncate_lines(text, max_lines, label=kernel_path.name)
    selected: list[str] = []
    for idx, line in enumerate(lines, start=1):
        if idx in hot_line_nums:
            selected.append(f"{idx:4d}| {line}")
        if len(selected) >= max_lines:
            break
    if not selected:
        return _truncate_lines(text, max_lines, label=kernel_path.name)
    return "\n".join(selected)


def _load_attempt_history(art: Path, limit: int = 5) -> list[dict[str, Any]]:
    path = art / "attempt_log.jsonl"
    if not path.is_file():
        legacy = art / "ab_trials.json"
        if legacy.is_file():
            try:
                data = json.loads(legacy.read_text(encoding="utf-8"))
                return list(data.get("trials") or [])[-limit:]
            except json.JSONDecodeError:
                return []
        return []
    rows: list[dict[str, Any]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return rows[-limit:]


def build_context_packet(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    *,
    evidence: dict[str, Any],
    baseline_metrics: dict[str, Any],
    baseline_score: float,
    attempt_history: list[dict[str, Any]] | None = None,
    candidate_metrics: dict[str, Any] | None = None,
    repo_root: Path | None = None,
) -> dict[str, Any]:
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    _, analysis = load_query_analysis(pipeline_config.repo_root, query, workload=workload)
    paths = resolve_workload_paths(pipeline_config, workload, query)
    edit_paths = resolve_optimization_edit_paths(pipeline_config, workload, query)
    kernel_path = pipeline_config.repo_root / edit_paths["primary_cpp"]
    kernel_review = analyze_kernel_source(kernel_path)

    is_aux = workload.get("workload_kind") == "aux_state"
    if is_aux:
        summary_text = json.dumps(analysis_for_optimization(analysis or {}), indent=2)
    else:
        summary_text = compact_base_analysis_for_optimization(analysis or {})

    profile_path = art / "profile_summary.json"
    baseline_path = art / "baseline_summary.json"
    profile_data: dict[str, Any] = {}
    baseline_data: dict[str, Any] = {}
    if profile_path.is_file():
        try:
            profile_data = json.loads(profile_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            profile_data = {}
    if baseline_path.is_file():
        try:
            baseline_data = json.loads(baseline_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            baseline_data = {}

    header_path = pipeline_config.repo_root / edit_paths["header"]
    package_path = pipeline_config.repo_root / edit_paths["secondary_cpp"]
    fused_path = paths.get("fused_scan_path")
    fused_full = pipeline_config.repo_root / fused_path if fused_path else None

    sig_parts = [
        f"// {edit_paths['header']}\n{_extract_signatures(header_path, _SECTION_LINE_CAPS['signatures'])}",
        f"// {edit_paths['primary_cpp']}\n{_extract_signatures(kernel_path, _SECTION_LINE_CAPS['signatures'])}",
        f"// {edit_paths['secondary_cpp']}\n{_extract_signatures(package_path, _SECTION_LINE_CAPS['signatures'])}",
    ]
    if fused_full and fused_full.is_file():
        sig_parts.append(
            f"// {fused_path}\n{_extract_signatures(fused_full, _SECTION_LINE_CAPS['signatures'])}"
        )

    history = attempt_history if attempt_history is not None else _load_attempt_history(art)
    failed = [h for h in history if not h.get("accepted") and not h.get("kept")][-_SECTION_LINE_CAPS["failed_hypotheses"] :]

    store_diag = extract_store_diagnostics(baseline_metrics)
    store_delta = None
    if candidate_metrics:
        store_delta = diagnostic_deltas(baseline_metrics, candidate_metrics)
    precedents = []
    if is_aux:
        root = repo_root or pipeline_config.repo_root
        _, analysis_full = load_query_analysis(root, query, workload=workload)
        precedents = retrieve_transformations(
            root,
            store_diagnostics=store_diag,
            analysis=analysis_full or {},
            failed_attempts=history,
        )

    packet = {
        "query": query,
        "baseline_score": baseline_score,
        "baseline_quick_metrics": baseline_metrics,
        "store_diagnostics": store_diag,
        "store_diagnostics_delta": store_delta,
        "retrieved_precedents": precedents,
        "summary": _truncate_lines(summary_text, _SECTION_LINE_CAPS["summary"], label="summary"),
        "hot_path_code": _hot_path_excerpt(kernel_path, kernel_review, _SECTION_LINE_CAPS["hot_path"]),
        "api_signatures": _truncate_lines(
            "\n\n".join(sig_parts), _SECTION_LINE_CAPS["signatures"] * 2, label="signatures"
        ),
        "measurements": _truncate_lines(
            json.dumps(
                {
                    "gate_baseline": compact_summary_for_prompt(baseline_data),
                    "gate_profile": compact_summary_for_prompt(profile_data),
                    "quick_baseline": baseline_metrics,
                },
                indent=2,
            ),
            _SECTION_LINE_CAPS["measurements"],
            label="measurements",
        ),
        "evidence": _truncate_lines(
            json.dumps(
                {
                    **evidence,
                    "store_diagnostic_tiers": organize_diagnostics_evidence(store_diag),
                },
                indent=2,
            ),
            _SECTION_LINE_CAPS["evidence"],
            label="evidence",
        ),
        "failed_hypotheses": [
            {
                "hypothesis": h.get("hypothesis", ""),
                "reason": h.get("failure_reason", h.get("reason", "")),
            }
            for h in failed
        ],
        "kernel_paths": {
            "header": edit_paths["header"],
            "kernel": edit_paths["primary_cpp"],
            "package": edit_paths["secondary_cpp"],
            "fused_scan": fused_path if not edit_paths.get("is_aux") else None,
            "cost_model": paths.get("cost_model_config") if edit_paths.get("is_aux") else None,
        },
    }
    return packet


def format_context_markdown(
    packet: dict[str, Any],
    *,
    max_lines: int = CONTEXT_MAX_LINES,
    max_chars: int = CONTEXT_MAX_CHARS,
) -> str:
    lines = [
        "# Optimization context packet",
        "",
        f"- baseline_score (`read_only_query_speedup`): **{float(packet.get('baseline_score') or 0):.4f}**",
        "",
        format_store_diagnostics_markdown(
            packet.get("store_diagnostics") or {},
            delta=packet.get("store_diagnostics_delta"),
            label="baseline",
        ),
        "",
        format_precedents_markdown(packet.get("retrieved_precedents") or []),
        "",
        "## Query / plan summary",
        packet.get("summary", ""),
        "",
        "## Hot-path code (excerpt)",
        "```cpp",
        packet.get("hot_path_code", ""),
        "```",
        "",
        "## API signatures",
        "```cpp",
        packet.get("api_signatures", ""),
        "```",
        "",
        "## Measurements",
        "```json",
        packet.get("measurements", "{}"),
        "```",
        "",
        "## Bottleneck evidence (tier-organized)",
        "```json",
        packet.get("evidence", "{}"),
        "```",
    ]
    failed = packet.get("failed_hypotheses") or []
    if failed:
        lines.extend(["", "## Recent failed hypotheses"])
        for row in failed:
            lines.append(f"- {row.get('hypothesis', '')} ({row.get('reason', '')})")
    for key, value in (packet.get("kernel_paths") or {}).items():
        if value:
            lines.append(f"- {key}: `{value}`")
    rendered = "\n".join(lines)
    return _truncate_chars(_truncate_lines(rendered, max_lines), max_chars)


def save_context_packet(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    packet: dict[str, Any],
) -> Path:
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    max_lines = int(getattr(pipeline_config, "optimization_context_max_lines", CONTEXT_MAX_LINES))
    max_chars = int(getattr(pipeline_config, "optimization_context_max_chars", CONTEXT_MAX_CHARS))
    md = format_context_markdown(packet, max_lines=max_lines, max_chars=max_chars)
    (art / "optimization_context.md").write_text(md, encoding="utf-8")
    (art / "optimization_context.json").write_text(json.dumps(packet, indent=2), encoding="utf-8")
    return art / "optimization_context.md"


def build_optimization_user_message(
    *,
    optimization_iter: int,
    attempt: int,
    context_markdown: str,
    bootstrap_markdown: str,
    workload_guidance: str = "",
    experimental_summary: str = "",
    hypothesis_phase: bool = False,
) -> str:
    """Single concise user message for optimization attempts."""
    guidance_block = f"\n\n## Workload guidance\n{workload_guidance}\n" if workload_guidance else ""
    experimental_block = (
        f"\n\n## Experimental branch diagnostic summary\n{experimental_summary}\n"
        if experimental_summary
        else ""
    )
    phase_note = (
        "Phase 1 (hypothesis only): output structured hypothesis JSON citing measured diagnostics. "
        "No file edits in this phase.\n"
        if hypothesis_phase
        else (
            "Hypothesis is locked — patch phase only. Edit the editable sources; "
            "no repository exploration.\n"
        )
    )
    return (
        f"Optimization iter {optimization_iter}, attempt {attempt}.\n"
        f"{phase_note}"
        "Sources are bootstrapped below — do not try to read files during patch phase.\n"
        f"{guidance_block}"
        f"{experimental_block}\n"
        f"{context_markdown}\n\n"
        f"{bootstrap_markdown}"
    )


def build_hot_path_digest(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    *,
    max_chars: int | None = None,
) -> str:
    """Compact hot-path + signatures for worker seed (not full bootstrap)."""
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    _, analysis = load_query_analysis(pipeline_config.repo_root, query, workload=workload)
    edit_paths = resolve_optimization_edit_paths(pipeline_config, workload, query)
    kernel_path = pipeline_config.repo_root / edit_paths["primary_cpp"]
    kernel_review = analyze_kernel_source(kernel_path)
    paths = resolve_workload_paths(pipeline_config, workload, query)
    header_path = pipeline_config.repo_root / edit_paths["header"]
    package_path = pipeline_config.repo_root / edit_paths["secondary_cpp"]
    fused_path = paths.get("fused_scan_path")
    fused_full = pipeline_config.repo_root / fused_path if fused_path else None

    sig_parts = [
        f"// {edit_paths['header']}\n{_extract_signatures(header_path, _SECTION_LINE_CAPS['signatures'])}",
        f"// {edit_paths['primary_cpp']}\n{_extract_signatures(kernel_path, _SECTION_LINE_CAPS['signatures'])}",
        f"// {edit_paths['secondary_cpp']}\n{_extract_signatures(package_path, _SECTION_LINE_CAPS['signatures'])}",
    ]
    if fused_full and fused_full.is_file():
        sig_parts.append(
            f"// {fused_path}\n{_extract_signatures(fused_full, _SECTION_LINE_CAPS['signatures'])}"
        )

    is_aux = workload.get("workload_kind") == "aux_state"
    if is_aux:
        summary_text = json.dumps(analysis_for_optimization(analysis or {}), indent=2)
    else:
        summary_text = compact_base_analysis_for_optimization(analysis or {})

    parts = [
        "# Optimization worker seed",
        "",
        "## Edit paths",
        f"- header: `{edit_paths['header']}`",
        f"- kernel: `{edit_paths['primary_cpp']}`",
        f"- package: `{edit_paths['secondary_cpp']}`",
        "",
        "## Summary",
        _truncate_lines(summary_text, _SECTION_LINE_CAPS["summary"], label="summary"),
        "",
        "## Hot-path excerpt",
        "```cpp",
        _hot_path_excerpt(kernel_path, kernel_review, _SECTION_LINE_CAPS["hot_path"]),
        "```",
        "",
        "## API signatures",
        "```cpp",
        "\n\n".join(sig_parts),
        "```",
    ]
    budget = max_chars or int(
        getattr(pipeline_config, "optimization_hot_path_digest_max_chars", 25_000) or 25_000
    )
    return _truncate_chars("\n".join(parts), budget)


def build_initial_worker_seed(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    *,
    evidence: dict[str, Any] | None = None,
    baseline_metrics: dict[str, Any] | None = None,
    baseline_score: float | None = None,
) -> str:
    """One-time semantic seed for optimization worker episode."""
    packet = build_context_packet(
        pipeline_config,
        workload,
        query,
        evidence=evidence or {},
        baseline_metrics=baseline_metrics or {},
        baseline_score=float(baseline_score or 0.0),
    )
    max_lines = int(getattr(pipeline_config, "optimization_context_max_lines", CONTEXT_MAX_LINES))
    max_chars = int(getattr(pipeline_config, "optimization_context_max_chars", CONTEXT_MAX_CHARS))
    context_md = format_context_markdown(packet, max_lines=max_lines, max_chars=max_chars)
    digest = build_hot_path_digest(pipeline_config, workload, query)
    from accel_pipeline.pack.repo_context_pack import format_seed_pointer

    pack_pointer = format_seed_pointer(pipeline_config.repo_root, max_preview_lines=24)
    return f"{pack_pointer}\n---\n\n{digest}\n\n---\n\n{context_md}"


def reconstruct_seed_from_artifacts(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    checkpoint: dict[str, Any] | None = None,
) -> str:
    """Rebuild compact worker seed after crash/--resume from authoritative artifacts."""
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    seed_path = art / "optimization_worker_seed.md"
    if seed_path.is_file() and not checkpoint:
        return seed_path.read_text(encoding="utf-8")

    parts = [
        "# Optimization worker resume context",
        "",
        "Code state restored from best-valid baseline on disk. Prior raw tool transcripts are not replayed.",
        "",
    ]
    ckpt = checkpoint or {}
    if ckpt.get("best_metrics"):
        parts.append("## Best metrics")
        parts.append("```json")
        parts.append(json.dumps(ckpt["best_metrics"], indent=2))
        parts.append("```")
    hypotheses = ckpt.get("recent_hypotheses") or []
    if hypotheses:
        parts.append("")
        parts.append("## Recent hypotheses")
        for row in hypotheses[-8:]:
            parts.append(
                f"- attempt {row.get('attempt')}: {row.get('bottleneck')} "
                f"→ {row.get('verdict')} ({row.get('improvement_pct')})"
            )
    if ckpt.get("experimental_branch_summary"):
        parts.append("")
        parts.append("## Experimental branch")
        parts.append("```json")
        parts.append(json.dumps(ckpt["experimental_branch_summary"], indent=2))
        parts.append("```")
    if ckpt.get("pending_feedback"):
        parts.append("")
        parts.append("## Pending gate feedback")
        parts.append(str(ckpt["pending_feedback"]))

    parts.append("")
    parts.append(build_hot_path_digest(pipeline_config, workload, query))
    return "\n".join(parts)
