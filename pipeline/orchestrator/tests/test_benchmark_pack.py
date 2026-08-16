from __future__ import annotations

from pathlib import Path

import pytest

from accel_pipeline.config import load_benchmark_pack, load_pipeline_config, load_workload
from accel_pipeline.paths import find_repo_root, query_artifacts_dir


REPO_ROOT = find_repo_root()


def test_load_sec_edgar_pack() -> None:
    pack = load_benchmark_pack("sec_edgar", REPO_ROOT)
    assert pack["benchmark_name"] == "sec_edgar"
    assert pack["function_prefix"] == "accel_sec_edgar_q"
    assert "sf10" not in pack.get("post_optimization_gates", [])
    assert "fairness" in pack.get("post_optimization_gates", [])
    assert "snapshot_paths" in pack
    assert any("fused_scan" in p for p in pack["snapshot_paths"])
    assert any("duckdb_scan_adapter" in p for p in pack["write_paths"])


def test_load_tpch_pack() -> None:
    pack = load_benchmark_pack("tpch", REPO_ROOT)
    assert pack["benchmark_name"] == "tpch"
    assert "sf10" in pack.get("post_optimization_gates", [])


def test_sec_edgar_workload_merge() -> None:
    workload = load_workload("sec_edgar_q6", REPO_ROOT)
    assert workload["function_name"] == "accel_sec_edgar_q6"
    assert workload["create_fn"] == "CreateSecEdgarQ6FunctionSet"
    assert workload["db_env_var"] == "ACCEL_SEC_EDGAR_DB"
    assert workload["post_audit_gates"] == ["fairness"]


def test_sec_edgar_reference_sql_exists() -> None:
    for name in ("sec_edgar_q1", "sec_edgar_q2", "sec_edgar_q3", "sec_edgar_q4", "sec_edgar_q6", "sec_edgar_q24"):
        workload = load_workload(name, REPO_ROOT)
        sql_path = Path(workload["reference_sql_path"])
        assert sql_path.is_file(), f"missing SQL for {name}: {sql_path}"


def test_artifact_namespace() -> None:
    workload = load_workload("sec_edgar_q1", REPO_ROOT)
    art = query_artifacts_dir(REPO_ROOT, 1, workload=workload)
    assert art == REPO_ROOT / "pipeline" / "artifacts" / "sec_edgar" / "q1"


def test_apply_workload_overrides_merges_write_paths() -> None:
    pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
    workload = load_workload("sec_edgar_q1", REPO_ROOT)
    from accel_pipeline.config import apply_workload_overrides

    merged = apply_workload_overrides(pipeline_config, workload)
    assert any("sec_edgar" in p for p in merged.write_paths)


def test_apply_workload_overrides_merges_aux_stage_max_turns() -> None:
    from accel_pipeline.config import apply_workload_overrides

    pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
    workload = load_workload("tpch_q18_aux", REPO_ROOT)
    merged = apply_workload_overrides(pipeline_config, workload)
    assert merged.stage_max_turns["analysis"] == 24
    assert merged.stage_max_turns["implementation"] == 0
    assert merged.stage_max_turns["optimization"] == 0
    assert merged.optimization_max_turns_per_episode == 0
    assert merged.optimization_max_attempts_per_iteration == 8
    assert merged.optimization_consecutive_fail_stop == 0
    assert merged.optimization_max_turns_patch == 0
    assert merged.optimization_max_turns_unified == 0
    assert merged.optimization_context_max_chars == 80000


def test_aux_workload_allows_cmake_edits() -> None:
    from accel_pipeline.config import apply_workload_overrides

    pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
    workload = load_workload("tpch_q9_aux", REPO_ROOT)
    merged = apply_workload_overrides(pipeline_config, workload)
    assert "CMakeLists.txt" in merged.write_paths
    assert any("calibrate_q*_aux_cost_model.cpp" in p for p in merged.write_paths)
    assert any("cross_accel_helpers.cpp" in p for p in merged.write_paths)
    assert any(
        "duckdb_aux_table_functions.cpp" in p for p in merged.write_paths
    ), "agent must be able to register DuckDB SQL aux table functions"


def test_aux_write_paths_allow_duckdb_table_function_registration() -> None:
    from accel_pipeline.config import apply_workload_overrides
    from accel_pipeline.tools.repo_tools import ToolContext, _is_writable

    pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
    workload = load_workload("tpch_q2_aux", REPO_ROOT)
    merged = apply_workload_overrides(pipeline_config, workload)
    ctx = ToolContext(pipeline_config=merged, query=2, stage="implementation")
    target = REPO_ROOT / "src/stateful/duckdb/duckdb_aux_table_functions.cpp"
    assert target.is_file()
    assert _is_writable(ctx, target)
