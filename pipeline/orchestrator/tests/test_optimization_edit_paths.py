"""Tests for resolve_optimization_edit_paths."""

from __future__ import annotations

from accel_pipeline.config import (
    PipelineConfig,
    load_workload,
    resolve_optimization_edit_paths,
)
from accel_pipeline.paths import find_repo_root


def test_aux_optimization_edit_paths_point_at_aux_module():
    repo = find_repo_root()
    workload = load_workload("tpch_q18_aux", repo, engine="duckdb")
    pc = PipelineConfig(
        repo_root=repo,
        pipeline_root=repo / "pipeline",
        build_dir=repo / "build-release",
        tpch_db_sf1=repo.parent / "accelerator-codegen-duckdb" / "tpch_sf1.duckdb",
    )
    edit = resolve_optimization_edit_paths(pc, workload, 18)
    assert edit["is_aux"] is True
    eng = workload["engine_name"]
    assert edit["primary_cpp"] == f"accelerators/tpch/q18/{eng}/aux/module.cpp"
    assert edit["header"] == f"accelerators/tpch/q18/{eng}/aux/module.hpp"
    assert edit["secondary_cpp"] == f"accelerators/tpch/q18/{eng}/aux/finalize.cpp"
    assert "accelerators/tpch/q18/config/q18_aux_cost_model_duckdb.json" in edit["all_paths"]
    assert "base_kernel_path" not in edit  # an aux accelerator has no stateless kernel beside it
