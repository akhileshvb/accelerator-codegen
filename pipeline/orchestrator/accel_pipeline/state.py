from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field
from pathlib import Path

from accel_pipeline.config import load_workload, query_artifacts_dir


STAGES = ("analysis", "test_suite", "implementation", "optimization", "audit")
# stage names of older pipeline_state.json files
STAGE_ALIASES = {"adversarial_tests": "test_suite"}


@dataclass
class PipelineState:
    query: int
    workload: str
    engine: str
    stage: str = "analysis"
    optimization_iter: int = 0
    session_id: str = ""
    # "" while running or finished; "stopped:<reason>" after a clean stop (--resume continues)
    status: str = ""
    structural_reanalysis_count: int = 0
    last_gate: dict[str, str] = field(default_factory=dict)

    @classmethod
    def load(cls, path: Path) -> PipelineState:
        data = json.loads(path.read_text(encoding="utf-8"))
        known = {f.name for f in cls.__dataclass_fields__.values()}
        filtered = {k: v for k, v in data.items() if k in known}
        filtered["stage"] = STAGE_ALIASES.get(filtered.get("stage", ""), filtered.get("stage", "analysis"))
        return cls(**filtered)

    def save(self, path: Path) -> None:
        path.write_text(json.dumps(asdict(self), indent=2), encoding="utf-8")


def state_path(repo_root: Path, query: int, workload_name: str, engine: str) -> Path:
    workload = load_workload(workload_name, repo_root, engine=engine)
    return query_artifacts_dir(repo_root, query, workload=workload) / "pipeline_state.json"


def load_or_create(repo_root: Path, query: int, workload: str, engine: str, resume: bool) -> PipelineState:
    path = state_path(repo_root, query, workload, engine)
    if resume and path.is_file():
        return PipelineState.load(path)
    state = PipelineState(query=query, workload=workload, engine=engine, session_id=f"q{query}-{engine}")
    state.save(path)
    return state


def advance_stage(state: PipelineState, repo_root: Path) -> None:
    idx = STAGES.index(state.stage)
    if idx + 1 < len(STAGES):
        state.stage = STAGES[idx + 1]
    state.save(state_path(repo_root, state.query, state.workload, state.engine))
