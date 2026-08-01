from __future__ import annotations

from pathlib import Path
from typing import Any

import yaml

from accel_pipeline.paths import pipeline_root


def load_kernel_inspiration_catalog(repo_root: Path) -> list[dict[str, Any]]:
    path = pipeline_root(repo_root) / "config" / "kernel_inspiration.yaml"
    if not path.is_file():
        return []
    with path.open(encoding="utf-8") as handle:
        data = yaml.safe_load(handle) or {}
    return list(data.get("categories") or [])


def format_kernel_inspiration_section(
    repo_root: Path,
    *,
    workload_refs: list[Any] | None = None,
    stage: str = "",
    compact: bool = False,
) -> str:
    """Build a prompt section pointing agents at mature kernels across the repo."""
    if compact:
        lines = [
            "## Kernel inspiration",
            "",
            "Read `pipeline/config/kernel_inspiration.yaml` and open 1–2 relevant files "
            "with `read_file` (match your bottleneck: fused scan, dict fast paths, fixed slots).",
        ]
        extra = [str(x) for x in (workload_refs or []) if x]
        if extra:
            lines.append("")
            lines.append("Workload hints: " + ", ".join(f"`{p}`" for p in extra[:4]))
        return "\n".join(lines)

    categories = load_kernel_inspiration_catalog(repo_root)
    lines = [
        "## Kernel inspiration (read across the repo)",
        "",
        "Before implementing or optimizing, **read** relevant existing accelerators in this repository. "
        "Port patterns that match your bottleneck — do not reinvent from scratch.",
        "",
    ]
    if stage in ("implementation", "optimization"):
        lines.append(
            "Use `read_file` / `grep` on the paths below. Prefer techniques that appear in multiple "
            "mature queries (fused scan, dictionary fast paths, fixed slots)."
        )
        lines.append("")

    for category in categories:
        name = str(category.get("name", "Patterns"))
        summary = str(category.get("summary", "")).strip()
        paths = [str(p) for p in category.get("paths") or []]
        if not paths:
            continue
        lines.append(f"### {name}")
        if summary:
            lines.append(summary)
        for rel in paths:
            full = repo_root / rel
            marker = "" if full.is_file() else " (create or adapt)"
            lines.append(f"- `{rel}`{marker}")
        lines.append("")

    extra = [str(x) for x in (workload_refs or []) if x]
    if extra:
        lines.append("### Workload-specific references")
        for rel in extra:
            lines.append(f"- `{rel}`")
        lines.append("")

    lines.append(
        "Also browse `src/generated/**/q*_kernel.cpp`, optional `q*_fused_scan.cpp`, and "
        "`benchmarks/**/benchmark_q*.cpp` for harness and hot-path patterns."
    )
    return "\n".join(lines)
