from __future__ import annotations

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
