from __future__ import annotations

from pathlib import Path


def find_repo_root(start: Path | None = None) -> Path:
    cur = (start or Path.cwd()).resolve()
    for parent in [cur, *cur.parents]:
        if (parent / "CMakeLists.txt").is_file() and (parent / "pipeline").is_dir():
            return parent
    raise FileNotFoundError("could not locate accelerator-codegen repo root")


def pipeline_root(repo_root: Path | None = None) -> Path:
    return (repo_root or find_repo_root()) / "pipeline"


def query_artifacts_dir(repo_root: Path, query_number: int) -> Path:
    path = pipeline_root(repo_root) / "artifacts" / f"q{query_number}"
    path.mkdir(parents=True, exist_ok=True)
    return path
