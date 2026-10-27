"""Engine_name parameterization for query-accelerator workloads."""

from __future__ import annotations

from pathlib import Path

import pytest

from accel_pipeline.config import (
    apply_workload_overrides,
    load_workload,
    resolve_workload_paths,
    workload_supported_engines,
    load_pipeline_config,
)
from accel_pipeline.gates import build_gate_context
from accel_pipeline.prompts import render_prompt


REPO_ROOT = Path(__file__).resolve().parents[3]


def _duckdb_workload(name: str) -> dict:
    return load_workload(name, REPO_ROOT, engine="duckdb")


def _postgres_workload(name: str) -> dict:
    return load_workload(name, REPO_ROOT, engine="postgres")


def test_duckdb_run_gets_the_duckdb_host():
    w = _duckdb_workload("tpch_q6")
    assert w["engine_name"] == "duckdb"
    assert w["engine_host"]["smoke_ready"] is True
    assert w["engine_host"]["differential_binary"] == "test_duckdb_tpch_differential"
    assert "duckdb" in w["engine_host"]["register_path"]


def test_engine_is_required_when_the_workload_supports_several():
    with pytest.raises(ValueError, match="pick one"):
        load_workload("tpch_q3", REPO_ROOT)


def test_single_engine_workload_needs_no_engine():
    assert load_workload("sec_edgar_q1", REPO_ROOT)["engine_name"] == "duckdb"


def test_unsupported_engine_is_rejected():
    with pytest.raises(ValueError, match="does not support engine 'mysql'"):
        load_workload("tpch_q3", REPO_ROOT, engine="mysql")
    with pytest.raises(ValueError, match="does not support engine 'postgres'"):
        load_workload("sec_edgar_q1", REPO_ROOT, engine="postgres")


def test_base_supports_duckdb_and_postgres_aux_adds_mysql():
    assert workload_supported_engines("tpch_q3", REPO_ROOT) == ["duckdb", "postgres"]
    assert workload_supported_engines("tpch_q3_aux", REPO_ROOT) == ["duckdb", "postgres", "mysql"]


def test_postgres_run_gets_the_agent_buildable_host():
    w = _postgres_workload("tpch_q3")
    assert w["engine_host"]["differential_binary"] == "test_postgres_tpch_differential"
    assert "duckdb_aux_table_functions.cpp" not in (w.get("write_paths") or [])


def test_aux_duckdb_registration_path():
    w = _duckdb_workload("tpch_q14_aux")
    assert w["engine_host"]["aux_register_path"] == "accelerators/tpch/q14/duckdb/aux/register.cpp"


def test_per_engine_fairness_search_paths():
    for engine in ("duckdb", "postgres"):
        paths = load_workload("tpch_q3", REPO_ROOT, engine=engine)["fairness"]["kernel_search_paths"]
        assert paths and all(f"/q{{N}}/{engine}/" in p for p in paths)


def test_gate_context_includes_engine_for_base_and_aux():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    base_ctx = build_gate_context(pc, _duckdb_workload("tpch_q6"), 6)
    aux_ctx = build_gate_context(pc, _duckdb_workload("tpch_q14_aux"), 14)
    assert base_ctx["engine_name"] == "duckdb"
    assert aux_ctx["engine_name"] == "duckdb"
    assert base_ctx["engine_host"]["dataset_env"] == "ACCEL_DB"
    assert aux_ctx["engine_host"]["differential_binary"] == "test_duckdb_tpch_differential"


def test_gate_context_postgres():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    ctx = build_gate_context(pc, _postgres_workload("tpch_q3"), 3)
    assert ctx["engine_name"] == "postgres"
    assert "postgres" in ctx["engine_host"]["differential_binary"]


def test_postgres_keeps_full_post_gates():
    from accel_pipeline.stages.base import workload_post_implementation_gates

    w = _postgres_workload("tpch_q3_aux")
    gates = workload_post_implementation_gates(w)
    assert gates == ["aux_unit", "prims_tests", "aux_exit"]
    assert w["engine_host"]["multi_conn_target"] == "test_postgres_aux_multi_connection_stress"


def test_apply_workload_overrides_strips_duckdb_tf_for_postgres():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    merged = apply_workload_overrides(pc, _postgres_workload("tpch_q3_aux"))
    assert "backends/duckdb/api/duckdb_aux_table_functions.cpp" not in merged.write_paths
    assert not any(p.startswith("backends/") for p in merged.write_paths if "{engine}" not in p)
    assert not any("postgres" in p for p in merged.write_paths)  # its harness is the backend's


def test_duckdb_correctness_gate_context_db_env(monkeypatch):
    """Regression: duckdb correctness still exposes ACCEL_TPCH_DB + dataset_db."""
    monkeypatch.setenv("ACCEL_TPCH_DB", "/data/tpch_sf1.duckdb")
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    ctx = build_gate_context(pc, _duckdb_workload("tpch_q6"), 6)
    assert ctx["engine_name"] == "duckdb"
    assert ctx["db_env_var"] == "ACCEL_TPCH_DB"
    assert str(ctx["dataset_db"]).endswith(".duckdb")
    assert ctx["engine_host"]["smoke_ready"] is True


def test_engine_targets_follow_the_engines_harness_convention():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    ddb = resolve_workload_paths(pc, _duckdb_workload("tpch_q6_aux"), 6)
    pg = resolve_workload_paths(pc, _postgres_workload("tpch_q6_aux"), 6)
    my = resolve_workload_paths(pc, load_workload("tpch_q6_aux", REPO_ROOT, engine="mysql"), 6)
    assert ddb["aux_bench_target"] == "benchmark_duckdb_tpch_aux"
    assert ddb["aux_calibrate_target"] == "calibrate_duckdb_tpch_aux_cost_model"
    assert ddb["cost_model_config"] == "accelerators/tpch/q6/config/q6_aux_cost_model_duckdb.json"
    assert pg["aux_bench_target"] == "benchmark_postgres_tpch_aux"
    assert pg["aux_calibrate_target"] == "calibrate_postgres_tpch_aux_cost_model"
    assert pg["cost_model_config"] == "accelerators/tpch/q6/config/q6_aux_cost_model_postgres.json"
    assert my["aux_bench_target"] == "benchmark_mysql_tpch_aux"
    assert my["cost_model_config"] == "accelerators/tpch/q6/config/q6_aux_cost_model_mysql.json"
    # cost_model.hpp names one query-wide location, whatever the engine.
    assert pg["cost_model_default"] == ddb["cost_model_default"] == "accelerators/tpch/q6/config/q6_aux_cost_model.json"


def test_registration_is_per_query():
    """Queries register from their own directory; the backend names no query."""
    from accel_pipeline.config import engine_host_conventions

    for engine in ("duckdb", "postgres", "mysql"):
        host = engine_host_conventions(engine, "tpch", 7)
        for key in ("register_path", "aux_register_path"):
            assert host[key].startswith(f"accelerators/tpch/q7/{engine}/"), (engine, key, host[key])
        assert host["registry"]


def test_engines_have_disjoint_artifact_trees():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    dirs = {}
    for engine in ("duckdb", "postgres", "mysql"):
        w = load_workload("tpch_q3_aux", REPO_ROOT, engine=engine)
        dirs[engine] = Path(build_gate_context(pc, w, 3)["artifacts_dir"])
        assert dirs[engine].name == engine and dirs[engine].parent.name == "q3"
    assert len(set(dirs.values())) == 3


def test_rendered_prompt_points_at_the_runs_engine_tree():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    texts = {
        e: render_prompt(pc, "01_analysis.md", load_workload("tpch_q3_aux", REPO_ROOT, engine=e), 3, "analysis")
        for e in ("duckdb", "postgres")
    }
    assert "pipeline/artifacts/tpch/q3/duckdb" in texts["duckdb"]
    assert "pipeline/artifacts/tpch/q3/postgres" not in texts["duckdb"]
    assert "pipeline/artifacts/tpch/q3/postgres" in texts["postgres"]
    assert "pipeline/artifacts/tpch/q3/duckdb" not in texts["postgres"]


def test_every_workload_loads_for_each_engine_it_lists():
    names = sorted(
        [p.stem for p in (REPO_ROOT / "pipeline/config/workloads").glob("*.yaml")
         if not p.stem.startswith("engine_") and "template" not in p.stem and p.stem != "tpch_queries"]
        + [f"tpch_q{n}{kind}" for n in range(1, 23) for kind in ("", "_aux")]
    )
    assert names
    for name in names:
        for engine in workload_supported_engines(name, REPO_ROOT):
            assert load_workload(name, REPO_ROOT, engine=engine)["engine_name"] == engine


def test_every_engine_follows_one_harness_naming_convention():
    from accel_pipeline.config import engine_host_conventions

    for engine in ("duckdb", "postgres", "mysql"):
        host = engine_host_conventions(engine, "tpch", 7)
        assert host["differential_binary"] == f"test_{engine}_tpch_differential"
        assert host["differential_source"] == f"backends/{engine}/tests/harness/test_{engine}_differential.cpp"
        assert host["multi_conn_target"] == f"test_{engine}_aux_multi_connection_stress"
        assert host["cross_accel_target"] == f"test_{engine}_cross_accel"
        assert host["bench_target"] == f"benchmark_{engine}_tpch_aux"
        assert host["calibrate_target"] == f"calibrate_{engine}_tpch_aux_cost_model"
