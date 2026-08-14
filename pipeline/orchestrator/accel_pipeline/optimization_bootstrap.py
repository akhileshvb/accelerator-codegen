"""Pre-load editable sources into the optimization agent message (no read_file churn)."""

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
        "## Editable sources (bootstrap — full contents, do not call read_file)",
        "",
        "All editable files are inlined below. Edit with `write_file` or `apply_patch` "
        "using these line numbers. **Do not re-read files** — the orchestrator refreshes "
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


def format_reference_aux_index(
    repo_root: Path,
    *,
    benchmark: str = "tpch",
    exclude_query: int | None = None,
) -> str:
    """List other aux modules the agent can explore via rip_grep / ast_grep."""
    pattern = f"src/generated/{benchmark}/q*/aux/module.cpp"
    paths = sorted(repo_root.glob(pattern))
    lines = [
        "## Reference aux accelerators (read-only — use `rip_grep` / `ast_grep`, not read_file)",
        "",
        "Explore patterns from other queries when redesigning aux-state layout, partitions, "
        "encode/decode, or maintenance. **Ambitious structural changes are encouraged** when "
        "evidence supports them (new partitions, dense layouts, alternate invalidation, etc.).",
        "",
    ]
    count = 0
    for path in paths:
        rel = path.relative_to(repo_root)
        parts = rel.parts
        q_num = None
        for part in parts:
            if part.startswith("q") and part[1:].isdigit():
                q_num = int(part[1:])
                break
        if exclude_query is not None and q_num == exclude_query:
            continue
        lines.append(f"- `{rel}` (Q{q_num})")
        count += 1
    if count == 0:
        lines.append("(no other aux modules found)")
    lines.append("")
    lines.append(
        "Example: `rip_grep('NationNamePartition', path='src/generated/tpch/q18/aux', context=5)`"
    )
    lines.append(
        "Example: `ast_grep('class $C { $$$ }', path='src/generated/tpch/q14/aux', lang='cpp')`"
    )
    return "\n".join(lines)


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
    benchmark = str(workload.get("benchmark_name", "tpch"))
    q = int(workload.get("query_number", query))
    reference_index = format_reference_aux_index(
        repo_root, benchmark=benchmark, exclude_query=q
    )
    markdown = (
        format_bootstrap_markdown(sources, max_chars=max_chars)
        + "\n\n"
        + reference_index
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
