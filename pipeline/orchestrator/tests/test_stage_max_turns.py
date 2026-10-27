from __future__ import annotations

from accel_pipeline.config import find_repo_root
from accel_pipeline.stages.base import resolve_optimization_max_turns, resolve_stage_max_turns


def test_resolve_stage_max_turns_unlimited_when_zero() -> None:
    assert resolve_stage_max_turns({"implementation": 0}, "implementation") == 10_000
    assert resolve_stage_max_turns({"implementation": 12}, "implementation") == 12


def test_resolve_optimization_max_turns_unlimited_when_zero() -> None:
    from accel_pipeline.config import PipelineConfig

    repo = find_repo_root()
    pc = PipelineConfig(
        repo_root=repo,
        pipeline_root=repo / "pipeline",
        build_dir=repo / "build-release",
        tpch_db_sf1=repo.parent / "accelerator-codegen-duckdb" / "tpch_sf1.duckdb",
        tpch_db_sf10=repo.parent / "accelerator-codegen-duckdb" / "tpch_sf10.duckdb",
        optimization_max_turns_unified=0,
    )
    assert resolve_optimization_max_turns(pc, "unified") == 10_000
    pc.optimization_max_turns_unified = 50
    assert resolve_optimization_max_turns(pc, "unified") == 50
