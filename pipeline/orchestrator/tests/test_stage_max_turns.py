from __future__ import annotations

from accel_pipeline.config import load_pipeline_config, load_workload
from accel_pipeline.paths import find_repo_root
from accel_pipeline.stages.base import resolve_stage_max_turns
from accel_pipeline.stages.implementation import _implementation_user_message


def test_resolve_stage_max_turns_unlimited_when_zero() -> None:
    assert resolve_stage_max_turns({"implementation": 0}, "implementation") == 10_000
    assert resolve_stage_max_turns({"implementation": 12}, "implementation") == 12


def test_aux_implementation_resume_message() -> None:
    repo = find_repo_root()
    pipeline_config = load_pipeline_config(repo_root=repo)
    workload = load_workload("tpch_q18_aux", repo, engine="duckdb")
    message = _implementation_user_message(pipeline_config, workload, 18)
    assert "RESUME partial aux implementation" in message
    assert "test_tpch_q18_aux" in message
    assert "cmake --build build-release" in message
