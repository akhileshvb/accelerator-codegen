from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

from accel_pipeline.config import load_workload
from accel_pipeline.paths import query_artifacts_dir


STAGES = ("analysis", "implementation", "optimization", "audit")


@dataclass
class PipelineState:
    query: int
    workload: str
    stage: str = "analysis"
    optimization_iter: int = 0
    session_id: str = ""
    structural_reanalysis_count: int = 0
    last_gate: dict[str, str] = field(default_factory=dict)
    models_used: dict[str, str] = field(default_factory=dict)

    @classmethod
    def load(cls, path: Path) -> PipelineState:
        data = json.loads(path.read_text(encoding="utf-8"))
        known = {f.name for f in cls.__dataclass_fields__.values()}
        filtered = {k: v for k, v in data.items() if k in known}
        return cls(**filtered)

    def save(self, path: Path) -> None:
        path.write_text(json.dumps(asdict(self), indent=2), encoding="utf-8")


def _workload_dict(repo_root: Path, workload_name: str) -> dict[str, Any]:
    try:
        return load_workload(workload_name, repo_root)
    except FileNotFoundError:
        return {"benchmark_name": "tpch", "artifact_namespace": "tpch"}


def state_path(repo_root: Path, query: int, workload_name: str) -> Path:
    workload = _workload_dict(repo_root, workload_name)
    return query_artifacts_dir(repo_root, query, workload=workload) / "pipeline_state.json"


def load_or_create(repo_root: Path, query: int, workload: str, resume: bool) -> PipelineState:
    path = state_path(repo_root, query, workload)
    if resume and path.is_file():
        return PipelineState.load(path)
    state = PipelineState(query=query, workload=workload, session_id=f"q{query}")
    state.save(path)
    return state


def advance_stage(state: PipelineState, repo_root: Path) -> None:
    idx = STAGES.index(state.stage)
    if idx + 1 < len(STAGES):
        state.stage = STAGES[idx + 1]
    state.save(state_path(repo_root, state.query, state.workload))


def read_json_artifact(repo_root: Path, query: int, name: str, *, workload_name: str = "tpch_q3") -> dict[str, Any]:
    workload = _workload_dict(repo_root, workload_name)
    path = query_artifacts_dir(repo_root, query, workload=workload) / name
    if not path.is_file():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))
