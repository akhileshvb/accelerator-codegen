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
    *,
    cost_model_config: str,
) -> str:
    benchmark = str(workload.get("benchmark_name", "tpch"))
    engine = str(workload.get("engine_name") or "duckdb")
    q = f"q{query}"
    qdir = f"accelerators/{benchmark}/{q}"
    lines = [
        "## Implementation bootstrap (authoritative — do not re-discover by listing directories)",
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

    lines.extend(["", "### Templates & gates (only if needed after module.cpp exists)", ""])
    templates = [
        f"{qdir}/{engine}/base/single-threaded/kernel.cpp",
        f"{qdir}/{engine}/base/query.hpp",
        f"{qdir}/tests/test_{benchmark}_{q}.cpp",
        f"{qdir}/tests/{q}_manifest.hpp",
        "eval/tests/test_tpch_module.cpp",
        f"accelerators/{benchmark}/q14/bench/benchmark_q14.cpp",
        cost_model_config,
        f"accelerators/{benchmark}/q14/cost_model.hpp",
        f"accelerators/{benchmark}/q14/layout.hpp",
        f"accelerators/{benchmark}/q14/duckdb/aux/register.cpp",
        "eval/tests/test_support/cross_accel_helpers.cpp",
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
            "1. Read the reference aux paths above once (they are the layout to mirror).",
            "2. **Immediately** write `module.hpp`, then `module.cpp`, then other `files_to_create`.",
            "3. Unit tests: none to write (shared ModuleRuntime test); make the catalog probe/arguments follow `unit_test_contract.md` + the predicate checklist above.",
            "4. Do **not** list the repo root, re-read analysis.json, or search for q14/q1 layouts after step 1.",
            "5. Search several files with `rg`; read a single file with `sed -n`.",
            "6. Build/test only after all primary sources exist; **register everywhere** (mirror peer-query wiring); finish only after MCP `run_correctness` **and** `run_multi_connection` both pass.",
            "",
        ]
    )
    return "\n".join(lines)
