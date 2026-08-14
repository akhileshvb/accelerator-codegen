"""Evidence-driven retrieval of prior successful transformations (non-prescriptive)."""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any

import yaml

from accel_pipeline.optimization_signals import extract_store_diagnostics

CATALOG_REL = "pipeline/catalog/aux_transformations.yaml"
MAX_PRECEDENTS = 3
MAX_EXCERPT_CHARS = 10_000
MAX_EXCERPT_LINES = 120

_RETRIEVAL_DISCLAIMER = (
    "Precedents are historical; legality and fit must be judged against this query's "
    "correctness_facts and constraints. Catalog matching is not proof the same design applies."
)


def _catalog_path(repo_root: Path) -> Path:
    return repo_root / CATALOG_REL


def load_transformation_catalog(repo_root: Path) -> list[dict[str, Any]]:
    path = _catalog_path(repo_root)
    if not path.is_file():
        return []
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    if isinstance(data, list):
        return [entry for entry in data if isinstance(entry, dict)]
    return []


def _problem_signals(diagnostics: dict[str, Any], analysis: dict[str, Any] | None) -> set[str]:
    signals: set[str] = set()
    diag = extract_store_diagnostics(diagnostics)
    slots = float(diag.get("validity_slots_scanned_median") or 0.0)
    parts_read = float(diag.get("partitions_read_per_query_median") or 0.0)
    valid_parts = float(diag.get("valid_partitions") or 0.0)
    if slots >= 1000:
        signals.add("high_validity_scan")
    if parts_read >= 10:
        signals.add("high_partition_read_fanout")
    if valid_parts >= 1000:
        signals.add("many_valid_partitions")
    if analysis:
        text = " ".join(str(x) for x in (analysis.get("likely_bottlenecks") or []))
        if re.search(r"string|varchar|brand|container|nation", text, re.I):
            signals.add("string_bind_param")
        if re.search(r"scalar|single.?row|aggregate", text, re.I):
            signals.add("scalar_output")
    return signals


def _entry_signals(entry: dict[str, Any]) -> set[str]:
    signals: set[str] = set()
    problem = str(entry.get("problem", "")).lower()
    if "validity" in problem or "scan" in problem:
        signals.add("high_validity_scan")
    if "fanout" in problem or "multi-partition" in problem or "partition" in problem:
        signals.add("high_partition_read_fanout")
    if "per-part" in problem or "many" in problem:
        signals.add("many_valid_partitions")
    if "string" in problem or "nation" in problem or "brand" in problem or "param" in problem:
        signals.add("string_bind_param")
    if "scalar" in problem:
        signals.add("scalar_output")
    effect = entry.get("measured_effect") or {}
    before_slots = float(effect.get("validity_slots_scanned_before") or 0.0)
    if before_slots >= 1000:
        signals.add("high_validity_scan")
    return signals


def _score_entry(entry: dict[str, Any], query_signals: set[str]) -> float:
    entry_sigs = _entry_signals(entry)
    if not query_signals or not entry_sigs:
        return 0.0
    overlap = len(query_signals & entry_sigs)
    return overlap / max(len(query_signals), 1)


def _read_excerpt(repo_root: Path, rel_path: str, *, max_lines: int = MAX_EXCERPT_LINES) -> str:
    path = repo_root / rel_path
    if not path.is_file():
        return ""
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    excerpt = "\n".join(f"{idx + 1:4d}| {line}" for idx, line in enumerate(lines[:max_lines]))
    if len(lines) > max_lines:
        excerpt += f"\n... [{len(lines) - max_lines} more lines truncated]"
    return excerpt


def retrieve_transformations(
    repo_root: Path,
    *,
    store_diagnostics: dict[str, Any] | None = None,
    analysis: dict[str, Any] | None = None,
    failed_attempts: list[dict[str, Any]] | None = None,
    limit: int = MAX_PRECEDENTS,
) -> list[dict[str, Any]]:
    catalog = load_transformation_catalog(repo_root)
    if not catalog:
        return []

    query_signals = _problem_signals(store_diagnostics or {}, analysis)
    for attempt in failed_attempts or []:
        metrics = attempt.get("candidate_metrics") or attempt.get("metrics") or {}
        query_signals |= _problem_signals(metrics, analysis)

    ranked = sorted(
        ((entry, _score_entry(entry, query_signals)) for entry in catalog),
        key=lambda item: item[1],
        reverse=True,
    )
    selected: list[dict[str, Any]] = []
    used_chars = 0
    for entry, score in ranked:
        if score <= 0 and selected:
            continue
        if len(selected) >= limit:
            break
        excerpts: list[str] = []
        for rel in entry.get("excerpt_paths") or []:
            if used_chars >= MAX_EXCERPT_CHARS:
                break
            text = _read_excerpt(repo_root, str(rel))
            if not text:
                continue
            remaining = MAX_EXCERPT_CHARS - used_chars
            if len(text) > remaining:
                text = text[:remaining] + "\n... [excerpt truncated]"
            excerpts.append(f"// {rel}\n{text}")
            used_chars += len(text)
        selected.append(
            {
                "id": entry.get("id"),
                "source_query": entry.get("source_query"),
                "problem": entry.get("problem"),
                "transformation": entry.get("transformation"),
                "measured_effect": entry.get("measured_effect"),
                "legality_constraints": entry.get("legality_constraints"),
                "retrieval_score": round(score, 3),
                "excerpts": excerpts,
            }
        )
    if not selected:
        selected = [
            {
                "id": entry.get("id"),
                "source_query": entry.get("source_query"),
                "problem": entry.get("problem"),
                "transformation": entry.get("transformation"),
                "measured_effect": entry.get("measured_effect"),
                "legality_constraints": entry.get("legality_constraints"),
                "retrieval_score": 0.0,
                "excerpts": [],
            }
            for entry in catalog[:limit]
        ]
    return selected


def format_precedents_markdown(precedents: list[dict[str, Any]]) -> str:
    if not precedents:
        return ""
    lines = [
        "## Retrieved prior transformations (precedents — adapt, combine, or reject)",
        "",
        f"_{_RETRIEVAL_DISCLAIMER}_",
        "",
    ]
    for entry in precedents:
        q = entry.get("source_query")
        title = entry.get("id", "precedent")
        lines.extend(
            [
                f"### Q{q}: {title}",
                f"- Problem: {entry.get('problem', '')}",
                f"- Transformation: {entry.get('transformation', '')}",
                f"- Measured effect: {entry.get('measured_effect', {})}",
                f"- Legality/constraints: {entry.get('legality_constraints', [])}",
            ]
        )
        for excerpt in entry.get("excerpts") or []:
            lines.extend(["", "```cpp", excerpt, "```"])
        lines.append("")
    return "\n".join(lines)
