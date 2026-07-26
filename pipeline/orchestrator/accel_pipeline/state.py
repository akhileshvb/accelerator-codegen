from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

from accel_pipeline.paths import query_artifacts_dir


STAGES = ("analysis", "implementation", "optimization", "audit")


@dataclass
class PipelineState:
    query: int
    workload: str
    stage: str = "analysis"
    optimization_iter: int = 0
    session_id: str = ""
    last_gate: dict[str, str] = field(default_factory=dict)
    models_used: dict[str, str] = field(default_factory=dict)

    @classmethod
    def load(cls, path: Path) -> PipelineState:
        data = json.loads(path.read_text(encoding="utf-8"))
        return cls(**data)

    def save(self, path: Path) -> None:
        path.write_text(json.dumps(asdict(self), indent=2), encoding="utf-8")


def state_path(repo_root: Path, query: int) -> Path:
    return query_artifacts_dir(repo_root, query) / "pipeline_state.json"


def load_or_create(repo_root: Path, query: int, workload: str, resume: bool) -> PipelineState:
    path = state_path(repo_root, query)
    if resume and path.is_file():
        return PipelineState.load(path)
    state = PipelineState(query=query, workload=workload, session_id=f"q{query}")
    state.save(path)
    return state


def advance_stage(state: PipelineState, repo_root: Path) -> None:
    idx = STAGES.index(state.stage)
    if idx + 1 < len(STAGES):
        state.stage = STAGES[idx + 1]
    state.save(state_path(repo_root, state.query))


def read_json_artifact(repo_root: Path, query: int, name: str) -> dict[str, Any]:
    path = query_artifacts_dir(repo_root, query) / name
    if not path.is_file():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))
