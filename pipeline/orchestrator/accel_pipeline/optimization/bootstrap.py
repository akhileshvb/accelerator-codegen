"""Pre-load editable sources into the optimization agent message (no re-reading churn)."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from accel_pipeline.config import PipelineConfig, resolve_optimization_edit_paths

BOOTSTRAP_MAX_CHARS_DEFAULT = 300_000


def _truncate_chars(text: str, max_chars: int) -> str:
    if max_chars <= 0 or len(text) <= max_chars:
        return text
    return text[: max_chars - 48] + f"\n... [truncated at {max_chars} chars]"


def load_editable_sources(
    repo_root: Path,
    edit_paths: dict[str, Any],
    *,
    max_lines_per_file: int = 0,
) -> list[dict[str, Any]]:
    """Load all editable paths with line numbers for patch/write tools."""
    sources: list[dict[str, Any]] = []
    for rel in edit_paths.get("all_paths") or []:
        path = repo_root / str(rel)
        if not path.is_file():
            continue
        raw = path.read_text(encoding="utf-8", errors="replace")
        lines = raw.splitlines()
        if max_lines_per_file > 0:
            lines = lines[:max_lines_per_file]
        numbered = "\n".join(f"{idx + 1}: {line}" for idx, line in enumerate(lines))
        sources.append(
            {
                "path": str(rel),
                "lines": len(lines),
                "bytes": len(raw.encode("utf-8")),
                "content": numbered,
            }
        )
    return sources


def format_bootstrap_markdown(
    sources: list[dict[str, Any]],
    *,
    max_chars: int = BOOTSTRAP_MAX_CHARS_DEFAULT,
) -> str:
    if not sources:
        return "## Editable sources (bootstrap)\n\n(no editable files found on disk)\n"

    parts = [
        "## Editable sources (bootstrap — full contents, do not re-read)",
        "",
        "All editable files are inlined below. Edit them in place using these line numbers. "
        "**Do not re-read files** — the orchestrator refreshes "
        "this block each attempt.",
        "",
    ]
    for entry in sources:
        path = entry["path"]
        parts.append(f"### `{path}` ({entry['lines']} lines)")
        parts.append("```cpp")
        parts.append(entry["content"])
        parts.append("```")
        parts.append("")

    return _truncate_chars("\n".join(parts), max_chars)


def build_optimization_bootstrap(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
) -> tuple[str, list[dict[str, Any]]]:
    repo_root = pipeline_config.repo_root
    edit_paths = resolve_optimization_edit_paths(pipeline_config, workload, query)
    max_lines = int(getattr(pipeline_config, "optimization_bootstrap_max_lines_per_file", 0) or 0)
    max_chars = int(
        getattr(pipeline_config, "optimization_bootstrap_max_chars", BOOTSTRAP_MAX_CHARS_DEFAULT)
        or BOOTSTRAP_MAX_CHARS_DEFAULT
    )
    sources = load_editable_sources(repo_root, edit_paths, max_lines_per_file=max_lines)
    from accel_pipeline.pack import format_seed_pointer

    # Prefer shared pack pointer over re-inlining the full peer aux index every attempt.
    pack_pointer = format_seed_pointer(repo_root, max_preview_lines=24)
    markdown = (
        format_bootstrap_markdown(sources, max_chars=max_chars)
        + "\n\n"
        + pack_pointer
    )
    return markdown, sources


def save_optimization_bootstrap(
    art: Path,
    markdown: str,
    sources: list[dict[str, Any]],
) -> Path:
    art.mkdir(parents=True, exist_ok=True)
    out = art / "optimization_bootstrap.md"
    out.write_text(markdown, encoding="utf-8")
    meta = art / "optimization_bootstrap_meta.json"
    meta.write_text(
        json.dumps(
            [{"path": s["path"], "lines": s["lines"], "bytes": s["bytes"]} for s in sources],
            indent=2,
        ),
        encoding="utf-8",
    )
    return out
