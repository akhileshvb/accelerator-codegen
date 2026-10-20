"""Engine_name parameterization for query-accelerator workloads."""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path

import pytest

from accel_pipeline.config import (
    apply_workload_overrides,
    load_pipeline_config,
    load_workload,
    resolve_workload_paths,
    workload_supported_engines,
)
from accel_pipeline.gate_context import build_gate_context
from accel_pipeline.prompts import render_prompt
from accel_pipeline.stages.implementation import _implementation_user_message


REPO_ROOT = Path(__file__).resolve().parents[3]


def _duckdb_workload(name: str) -> dict:
    return load_workload(name, REPO_ROOT, engine="duckdb")


def _postgres_workload(name: str) -> dict:
    return load_workload(name, REPO_ROOT, engine="postgres")


def test_duckdb_run_gets_the_duckdb_host():
    w = _duckdb_workload("tpch_q6")
    assert w["engine_name"] == "duckdb"
    assert w["engine_host"]["smoke_ready"] is True
    assert w["engine_host"]["differential_binary"] == "test_tpch_shared_differential"
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
    assert w["engine_host"]["agent_builds_query_host"] is True
    assert w["engine_host"]["differential_binary"] == "test_postgres_tpch_differential"
    assert "duckdb_aux_table_functions.cpp" not in (w.get("write_paths") or [])


def test_aux_duckdb_registration_path():
    w = _duckdb_workload("tpch_q14_aux")
    assert w["engine_host"]["aux_register_path"] == "accelerators/tpch/q14/duckdb/aux/register.cpp"


def test_per_engine_fairness_search_paths():
    assert any("postgres" in p for p in _postgres_workload("tpch_q3")["fairness"]["kernel_search_paths"])
    assert not any("postgres" in p for p in _duckdb_workload("tpch_q3")["fairness"]["kernel_search_paths"])


def test_gate_context_includes_engine_for_base_and_aux():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    base_ctx = build_gate_context(pc, _duckdb_workload("tpch_q6"), 6)
    aux_ctx = build_gate_context(pc, _duckdb_workload("tpch_q14_aux"), 14)
    assert base_ctx["engine_name"] == "duckdb"
    assert aux_ctx["engine_name"] == "duckdb"
    assert base_ctx["engine_host"]["dataset_env"] == "ACCEL_TPCH_DB"
    assert aux_ctx["engine_host"]["differential_binary"] == "test_tpch_shared_differential"


def test_gate_context_postgres():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    ctx = build_gate_context(pc, _postgres_workload("tpch_q3"), 3)
    assert ctx["engine_name"] == "postgres"
    assert ctx["engine_host"]["agent_builds_query_host"] is True
    assert "postgres" in ctx["engine_host"]["differential_binary"]


def test_postgres_implementation_prompt_mentions_bootstrap():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = _postgres_workload("tpch_q3")
    text = render_prompt(pc, "02_implementation.md", w, 3, "implementation")
    assert "query_host_bootstrap" in text or "bootstrap" in text.lower()
    assert "postgres" in text.lower()


def test_postgres_keeps_full_post_gates():
    from accel_pipeline.stages.gate_plan import workload_post_implementation_gates

    w = _postgres_workload("tpch_q3_aux")
    gates = workload_post_implementation_gates(w)
    assert "aux_correctness" in gates
    assert "aux_multi_connection" in gates
    assert "aux_bench_sweep" in gates
    assert w["engine_host"]["multi_conn_ctest_regex"] == "test_postgres_aux_multi_connection_stress"


def test_implementation_prompt_mentions_substrate_and_engine():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = _duckdb_workload("tpch_q6")
    text = render_prompt(pc, "02_implementation.md", w, 6, "implementation")
    assert "NativeScanSpec" in text or "substrate" in text.lower() or "engine_name" in text
    assert "duckdb" in text.lower()
    assert "Engine host" in text or "engine_name" in text


def test_aux_implementation_resume_uses_engine_differential():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = _postgres_workload("tpch_q3_aux")
    msg = _implementation_user_message(pc, w, 3)
    if "RESUME" in msg:
        assert "test_postgres_tpch_differential" in msg or "postgres" in msg.lower()

    w_ddb = _duckdb_workload("tpch_q3_aux")
    paths = resolve_workload_paths(pc, w_ddb, 3)
    module = pc.repo_root / paths["aux_state_subdir"] / "module.cpp"
    if module.is_file():
        msg_ddb = _implementation_user_message(pc, w_ddb, 3)
        assert "test_tpch_shared_differential" in msg_ddb


def test_apply_workload_overrides_strips_duckdb_tf_for_postgres():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    merged = apply_workload_overrides(pc, _postgres_workload("tpch_q3_aux"))
    assert "backends/duckdb/api/duckdb_aux_table_functions.cpp" not in merged.write_paths
    assert not any(p.startswith("backends/") for p in merged.write_paths if "{engine}" not in p)
    assert any("postgres" in p for p in merged.write_paths)


def test_duckdb_correctness_gate_context_db_env():
    """Regression: duckdb correctness still exposes ACCEL_TPCH_DB + dataset_db."""
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
    assert ddb["cost_model_config"] == "accelerators/tpch/q6/config/q6_aux_cost_model.json"
    assert pg["aux_bench_target"] == "benchmark_postgres_tpch_aux"
    assert pg["aux_calibrate_target"] == "calibrate_postgres_tpch_aux_cost_model"
    assert pg["cost_model_config"] == "accelerators/tpch/q6/config/q6_aux_cost_model_postgres.json"
    assert my["aux_bench_target"] == "benchmark_mysql_tpch_aux"
    assert my["cost_model_config"] == "accelerators/tpch/q6/config/q6_aux_cost_model_mysql.json"
    # cost_model.hpp names one query-wide location, whatever the engine.
    assert pg["cost_model_default"] == ddb["cost_model_default"] == ddb["cost_model_config"]


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


def test_prerequisite_is_skipped_for_engines_without_a_base(capsys):
    from accel_pipeline.prerequisites import ensure_base_accelerator_prerequisite

    pc = replace(load_pipeline_config(repo_root=REPO_ROOT), engine_name="mysql")
    assert ensure_base_accelerator_prerequisite(pc, "tpch_q3_aux", 3, dry_run=True) is False
    assert "no mysql variant" in capsys.readouterr().err


def test_every_workload_loads_for_each_engine_it_lists():
    names = sorted(
        p.stem
        for p in (REPO_ROOT / "pipeline/config/workloads").glob("*.yaml")
        if not p.stem.startswith("engine_") and "template" not in p.stem
    )
    assert names
    for name in names:
        for engine in workload_supported_engines(name, REPO_ROOT):
            assert load_workload(name, REPO_ROOT, engine=engine)["engine_name"] == engine
