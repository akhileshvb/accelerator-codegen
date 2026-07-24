from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from accel_pipeline.config import PipelineConfig
from accel_pipeline.paths import pipeline_root, query_artifacts_dir


def _read_text(path: Path, default: str = "") -> str:
    return path.read_text(encoding="utf-8") if path.is_file() else default


def _tail_lines(path: Path, n: int = 3) -> str:
    if not path.is_file():
        return ""
    lines = path.read_text(encoding="utf-8").strip().splitlines()
    return "\n".join(lines[-n:])


def build_retrieval_brief(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
) -> str:
    # TODO: Select handbook section by bottleneck signature (03_handbook/*.md) instead of full-file excerpt.
    # TODO: Rank playbook entries by query_shape / dominant_phase; inject top-K only.
    repo = pipeline_config.repo_root
    art = query_artifacts_dir(repo, query)
    analysis_path = art / "analysis.json"
    analysis = {}
    if analysis_path.is_file():
        analysis = json.loads(analysis_path.read_text(encoding="utf-8"))

    bottlenecks = analysis.get("likely_bottlenecks", [])
    playbook_path = pipeline_root(repo) / "artifacts" / "shared" / "optimization_playbook.md"
    playbook_excerpt = _read_text(playbook_path)[:2000]

    handbook_path = pipeline_root(repo) / "prompts" / "shared" / "handbook.md"
    handbook = _read_text(handbook_path)
    handbook_excerpt = handbook[:2500]

    lines = [
        "## Retrieval brief",
        f"Query: Q{query}",
        f"Bottlenecks: {json.dumps(bottlenecks)}",
        "",
        "### Playbook excerpt",
        playbook_excerpt,
        "",
        "### Handbook excerpt",
        handbook_excerpt,
    ]
    return "\n".join(lines)


def build_prompt_context(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    stage: str,
    optimization_iter: int = 0,
) -> dict[str, str]:
    repo = pipeline_config.repo_root
    pipe = pipeline_config.pipeline_root
    art = query_artifacts_dir(repo, query)

    reference_sql = ""
    ref_path = workload.get("reference_sql_path")
    if ref_path and Path(ref_path).is_file():
        reference_sql = Path(ref_path).read_text(encoding="utf-8")

    import yaml

    ctx: dict[str, str] = {
        "query_number": str(query),
        "repo_root": str(repo),
        "duckdb_root": str(pipeline_config.repo_root.parent / "accelerator-codegen-duckdb"),
        "reference_sql": reference_sql,
        "parameters_yaml": yaml.safe_dump(workload.get("parameters", [])),
        "output_schema_yaml": yaml.safe_dump(workload.get("output_schema", [])),
        "reference_accelerators": ", ".join(str(x) for x in workload.get("reference_accelerators", [])),
        "constraints": _read_text(pipe / "prompts" / "shared" / "constraints.md"),
        "profiling_conventions": _read_text(pipe / "prompts" / "shared" / "profiling_conventions.md"),
        "analysis_json": _read_text(art / "analysis.json", "{}"),
        "baseline_summary_json": _read_text(art / "baseline_summary.json", "{}"),
        "retrieval_brief": build_retrieval_brief(pipeline_config, workload, query),
        "experiment_log_excerpt": _tail_lines(art / "experiment_log.md", 5),
        "experiment_summary": _read_text(art / "experiment_log.md")[:3000],
        "handbook_excerpt": _read_text(pipe / "prompts" / "shared" / "handbook.md")[:2500],
        "optimization_iter": str(optimization_iter),
    }
    return ctx
