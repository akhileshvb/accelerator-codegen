"""Pre-rendered paths and workflow for aux-state implementation (reduces explore loops)."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from accel_pipeline.sql_predicates import format_predicate_bootstrap_section, load_reference_sql


def _rel(repo: Path, path: Path) -> str:
    try:
        return str(path.relative_to(repo))
    except ValueError:
        return str(path)


def _existing(repo: Path, rel: str) -> str | None:
    path = repo / rel
    return rel if path.is_file() else None


def build_implementation_bootstrap(
    repo: Path,
    workload: dict[str, Any],
    query: int,
    analysis: dict[str, Any],
) -> str:
    benchmark = str(workload.get("benchmark_name", "tpch"))
    q = f"q{query}"
    lines = [
        "## Implementation bootstrap (authoritative — do not re-discover via list_dir/glob)",
        "",
        "### Files to create (write these; `implementation_context_json` has semantics)",
        "",
    ]
    for item in analysis.get("files_to_create") or []:
        if isinstance(item, dict):
            path = item.get("path", "")
            role = item.get("role", "")
            lines.append(f"- `{path}` — {role}")
        else:
            lines.append(f"- `{item}`")

    lines.extend(["", "### Reference aux modules (read with `read_reference_files`, not chunked read_file)", ""])
    ref_paths: list[str] = []
    for ref in workload.get("reference_accelerators") or []:
        ref_str = str(ref)
        if ref_str.endswith(".cpp") or ref_str.endswith(".hpp"):
            if _existing(repo, ref_str):
                ref_paths.append(ref_str)
            continue
        for suffix in ("aux/module.hpp", "aux/module.cpp", "aux/finalize.hpp", "aux/finalize.cpp"):
            candidate = f"src/generated/{benchmark}/{ref_str}/{suffix}"
            if _existing(repo, candidate):
                ref_paths.append(candidate)
    if not ref_paths:
        for fallback_q in (14, 10, 3, 1):
            for suffix in ("aux/module.hpp", "aux/module.cpp"):
                candidate = f"src/generated/{benchmark}/q{fallback_q}/{suffix}"
                if _existing(repo, candidate):
                    ref_paths.append(candidate)
    for path in ref_paths[:6]:
        lines.append(f"- `{path}`")

    lines.extend(["", "### Templates & gates (only if needed after module.cpp exists)", ""])
    templates = [
        f"src/generated/{benchmark}/{q}/base/single-threaded/kernel.cpp",
        f"src/generated/{benchmark}/{q}/base/query.hpp",
        f"tests/single-threaded/test_tpch_{query}.cpp",
        f"tests/test_support/q{query}_manifest.hpp",
        f"tests/aux_state/test_shared_tpch_q14.cpp",
        f"benchmarks/tpch/single-threaded/benchmark_q14.cpp",
        f"config/q{query}_aux_cost_model.json",
        "include/accelerator_codegen/stateful/core/query_layouts.hpp",
        "tests/test_support/cross_accel_helpers.cpp",
        "pipeline/prompts/aux_state/shared/read_path_scaffold.md",
        "pipeline/prompts/aux_state/shared/unit_test_contract.md",
    ]
    for rel in templates:
        if _existing(repo, rel):
            lines.append(f"- `{rel}`")

    ref_sql_path = str(workload.get("reference_sql_path") or "")
    ref_sql = load_reference_sql(ref_sql_path)
    lines.extend(["", format_predicate_bootstrap_section(ref_sql, ref_sql_path)])

    lines.extend(
        [
            "### Turn discipline",
            "",
            "1. **One** `read_reference_files` call on the reference aux paths above (limit_per_file=500).",
            "2. **Immediately** `write_file` `module.hpp`, then `module.cpp`, then other `files_to_create`.",
            "3. Unit tests: follow `unit_test_contract.md` + the predicate checklist above (not invented polarity).",
            "4. Do **not** `list_dir` the repo root, re-read analysis.json, or glob for q14/q1 after step 1.",
            "5. `grep_context` requires a **file** path (not a directory). Use `search_repo` for multi-file search.",
            "6. Build/test only after all primary sources exist; **register everywhere** (mirror peer-query wiring); finish only after MCP `run_correctness` **and** `run_multi_connection` both pass.",
            "",
        ]
    )
    return "\n".join(lines)
