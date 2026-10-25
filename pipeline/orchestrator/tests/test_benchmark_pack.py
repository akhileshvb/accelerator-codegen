from __future__ import annotations

from pathlib import Path

import pytest

from accel_pipeline.config import load_benchmark_pack, load_pipeline_config, load_workload
from accel_pipeline.paths import find_repo_root, query_artifacts_dir


REPO_ROOT = find_repo_root()


def test_load_sec_edgar_pack() -> None:
    pack = load_benchmark_pack("sec_edgar", REPO_ROOT)
    assert pack["benchmark_name"] == "sec_edgar"
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
    assert art == REPO_ROOT / "pipeline" / "artifacts" / "sec_edgar" / "q1" / "duckdb"


def test_apply_workload_overrides_merges_write_paths() -> None:
    pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
    workload = load_workload("sec_edgar_q1", REPO_ROOT)
    from accel_pipeline.config import apply_workload_overrides

    merged = apply_workload_overrides(pipeline_config, workload)
    assert any("sec_edgar" in p for p in merged.write_paths)


def test_apply_workload_overrides_merges_aux_stage_max_turns() -> None:
    from accel_pipeline.config import apply_workload_overrides

    pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
    workload = load_workload("tpch_q18_aux", REPO_ROOT, engine="duckdb")
    merged = apply_workload_overrides(pipeline_config, workload)
    assert merged.stage_max_turns["analysis"] == 24
    assert merged.stage_max_turns["implementation"] == 0
    assert merged.stage_max_turns["optimization"] == 0
    assert merged.optimization_max_attempts_per_iteration == 8
    assert merged.optimization_consecutive_fail_stop == 0
    assert merged.optimization_max_turns_patch == 0
    assert merged.optimization_max_turns_unified == 0
    assert merged.optimization_context_max_chars == 80000


def test_aux_workload_never_edits_root_cmake() -> None:
    from accel_pipeline.config import apply_workload_overrides

    pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
    workload = load_workload("tpch_q9_aux", REPO_ROOT, engine="duckdb")
    merged = apply_workload_overrides(pipeline_config, workload)
    assert "CMakeLists.txt" not in merged.write_paths
    assert not any(p.startswith("cmake/") for p in merged.write_paths)
    assert "accelerators/tpch/q9/**" in merged.write_paths
    assert "eval/tests/test_support/**" in merged.write_paths


def test_aux_write_paths_register_from_query_dir_not_backend() -> None:
    """The query registers its table function from its own directory; the DuckDB
    backend's table-function file (backends/duckdb/**) is the engine_backend run's."""
    from accel_pipeline.config import apply_workload_overrides
    from accel_pipeline.write_ownership import write_allowed

    pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
    workload = load_workload("tpch_q2_aux", REPO_ROOT, engine="duckdb")
    merged = apply_workload_overrides(pipeline_config, workload)

    def writable(path: Path) -> bool:
        ok, _reason = write_allowed(
            REPO_ROOT, path, query=2, stage="implementation", write_paths=list(merged.write_paths),
            pipeline_config=merged,
        )
        return ok

    target = REPO_ROOT / "backends/duckdb/api/duckdb_aux_table_functions.cpp"
    assert target.is_file()
    assert not writable(target)
    assert writable(REPO_ROOT / "accelerators/tpch/q2/duckdb/aux/register.cpp")


def test_a_querys_workloads_run_one_after_another_per_engine():
    from accel_pipeline.pack.run import _chains_by_artifact_dir, parse_pack_queries

    pack = {"engines": ["engine_a", "engine_b"], "queries": [1, 6],
            "workload_template": ["tpch_q{q}_aux", "tpch_q{q}"]}
    chains = _chains_by_artifact_dir(parse_pack_queries(pack))
    assert len(chains) == 4
    assert all([e.workload for e in c] == [f"tpch_q{c[0].query}_aux", f"tpch_q{c[0].query}"] for c in chains)
