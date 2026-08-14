from __future__ import annotations

import json
from pathlib import Path
from typing import Any


def find_repo_root(start: Path | None = None) -> Path:
    cur = (start or Path.cwd()).resolve()
    for parent in [cur, *cur.parents]:
        if (parent / "CMakeLists.txt").is_file() and (parent / "pipeline").is_dir():
            return parent
    raise FileNotFoundError("could not locate accelerator-codegen repo root")


def pipeline_root(repo_root: Path | None = None) -> Path:
    return (repo_root or find_repo_root()) / "pipeline"


def workload_benchmark_name(workload: dict[str, Any] | None) -> str:
    if not workload:
        return "tpch"
    return str(workload.get("artifact_namespace") or workload.get("benchmark_name") or "tpch")


def query_artifacts_dir(
    repo_root: Path,
    query_number: int,
    *,
    workload: dict[str, Any] | None = None,
    benchmark_name: str | None = None,
) -> Path:
    bench = benchmark_name or workload_benchmark_name(workload)
    namespaced = pipeline_root(repo_root) / "artifacts" / bench / f"q{query_number}"
    legacy = pipeline_root(repo_root) / "artifacts" / f"q{query_number}"

    if bench == "tpch" and legacy.is_dir() and not namespaced.is_dir():
        legacy.mkdir(parents=True, exist_ok=True)
        return legacy

    namespaced.mkdir(parents=True, exist_ok=True)
    return namespaced


def gates_dir_for_query(artifacts_dir: Path) -> Path:
    return artifacts_dir / "gates"


def analysis_artifact_name(workload: dict[str, Any] | None) -> str:
    """Aux-state workloads use analysis.json; base accelerators use base_analysis.json."""
    if workload and workload.get("workload_kind") == "aux_state":
        return "analysis.json"
    return "base_analysis.json"


def analysis_artifact_path(
    repo_root: Path,
    query_number: int,
    *,
    workload: dict[str, Any] | None = None,
    benchmark_name: str | None = None,
) -> Path:
    art = query_artifacts_dir(repo_root, query_number, workload=workload, benchmark_name=benchmark_name)
    return art / analysis_artifact_name(workload)


def resolve_analysis_artifact_path(
    artifacts_dir: Path,
    workload: dict[str, Any] | None,
) -> Path:
    """Return the analysis artifact path, with legacy fallback for base workloads."""
    path = artifacts_dir / analysis_artifact_name(workload)
    if path.is_file():
        return path
    if workload and workload.get("workload_kind") == "aux_state":
        return path
    legacy = artifacts_dir / "analysis.json"
    if not legacy.is_file():
        return path
    try:
        data = json.loads(legacy.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return path
    if isinstance(data, dict) and "fitness_assessment" not in data:
        return legacy
    return path


def load_query_analysis(
    repo_root: Path,
    query_number: int,
    *,
    workload: dict[str, Any] | None = None,
) -> tuple[Path, dict[str, Any]]:
    art = query_artifacts_dir(repo_root, query_number, workload=workload)
    path = resolve_analysis_artifact_path(art, workload)
    if not path.is_file():
        return path, {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return path, {}
    return path, data if isinstance(data, dict) else {}
