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
    """artifacts/<bench>/q<N>/<engine>/ for accelerator runs (one per query x engine)."""
    if workload and workload.get("workload_kind") == "engine_backend":
        engine = str(workload.get("engine_name") or workload.get("artifact_namespace") or "engine")
        path = pipeline_root(repo_root) / "artifacts" / "engines" / engine
        path.mkdir(parents=True, exist_ok=True)
        return path

    bench = benchmark_name or workload_benchmark_name(workload)
    path = pipeline_root(repo_root) / "artifacts" / bench / f"q{query_number}"
    engine = str((workload or {}).get("engine_name") or "")
    if engine:
        path = path / engine
    path.mkdir(parents=True, exist_ok=True)
    return path


def engine_artifacts_dir(repo_root: Path, engine_name: str) -> Path:
    path = pipeline_root(repo_root) / "artifacts" / "engines" / engine_name
    path.mkdir(parents=True, exist_ok=True)
    return path


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


def load_query_analysis(
    repo_root: Path,
    query_number: int,
    *,
    workload: dict[str, Any] | None = None,
) -> tuple[Path, dict[str, Any]]:
    art = query_artifacts_dir(repo_root, query_number, workload=workload)
    path = art / analysis_artifact_name(workload)
    if not path.is_file():
        return path, {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return path, {}
    return path, data if isinstance(data, dict) else {}
