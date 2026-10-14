"""Engine_name parameterization for query-accelerator workloads."""

from __future__ import annotations

import importlib.util
from pathlib import Path

from accel_pipeline.config import (
    apply_workload_overrides,
    engine_host_conventions,
    load_pipeline_config,
    load_workload,
    _normalize_accelerator_engine,
)
from accel_pipeline.gate_context import build_gate_context
from accel_pipeline.prompts import render_prompt
from accel_pipeline.stages.implementation import _implementation_user_message


REPO_ROOT = Path(__file__).resolve().parents[3]


def test_accelerator_requires_explicit_engine_name():
    w = load_workload("tpch_q6", REPO_ROOT)
    assert w["engine_name"] == "duckdb"
    assert w["engine_host"]["smoke_ready"] is True
    assert w["engine_host"]["differential_binary"] == "test_tpch_shared_differential"
    assert "duckdb" in w["engine_host"]["register_path"]


def test_normalize_accelerator_rejects_missing_engine_name():
    import pytest

    with pytest.raises(ValueError, match="explicit engine_name"):
        _normalize_accelerator_engine({"benchmark_name": "tpch", "query_number": 1})


def test_tpch_q3_pins_postgres():
    w = load_workload("tpch_q3", REPO_ROOT)
    assert w["engine_name"] == "postgres"
    assert w["engine_host"]["agent_builds_query_host"] is True
    assert w["engine_host"]["differential_binary"] == "test_postgres_tpch_differential"


def test_aux_requires_explicit_engine_name_duckdb():
    w = load_workload("tpch_q6_aux", REPO_ROOT) if (REPO_ROOT / "pipeline/config/workloads/tpch_q6_aux.yaml").is_file() else None
    if w is None:
        w = load_workload("tpch_q14_aux", REPO_ROOT)
        w = dict(w)
        w["engine_name"] = "duckdb"
        w = _normalize_accelerator_engine(w)
    assert w["engine_name"] == "duckdb"
    assert w["engine_host"]["aux_register_path"].endswith("duckdb_aux_table_functions.cpp")


def test_explicit_postgres_engine_name_preserved():
    w = load_workload("tpch_q6", REPO_ROOT)
    w["engine_name"] = "postgres"
    w = _normalize_accelerator_engine(w)
    assert w["engine_name"] == "postgres"
    assert w["engine_host"]["smoke_ready"] is True
    assert w["engine_host"]["agent_builds_query_host"] is True
    assert w["engine_host"]["differential_binary"] == "test_postgres_tpch_differential"
    assert "duckdb_aux_table_functions.cpp" not in (w.get("write_paths") or [])


def test_gate_context_includes_engine_for_base_and_aux():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    base = load_workload("tpch_q6", REPO_ROOT)
    aux = load_workload("tpch_q14_aux", REPO_ROOT)
    aux = dict(aux)
    aux["engine_name"] = "duckdb"
    aux = _normalize_accelerator_engine(aux)
    base_ctx = build_gate_context(pc, base, 6)
    aux_ctx = build_gate_context(pc, aux, 14)
    assert base_ctx["engine_name"] == "duckdb"
    assert aux_ctx["engine_name"] == "duckdb"
    assert base_ctx["engine_host"]["dataset_env"] == "ACCEL_TPCH_DB"
    assert aux_ctx["engine_host"]["differential_binary"] == "test_tpch_shared_differential"


def test_gate_context_postgres_from_q3_yaml():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("tpch_q3", REPO_ROOT)
    ctx = build_gate_context(pc, w, 3)
    assert ctx["engine_name"] == "postgres"
    assert ctx["engine_host"]["agent_builds_query_host"] is True
    assert "postgres" in ctx["engine_host"]["differential_binary"]


def test_postgres_implementation_prompt_mentions_bootstrap():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("tpch_q3", REPO_ROOT)
    text = render_prompt(pc, "02_implementation.md", w, 3, "implementation")
    assert "query_host_bootstrap" in text or "bootstrap" in text.lower()
    assert "postgres" in text.lower()


def test_postgres_keeps_full_post_gates():
    from accel_pipeline.stages.gate_plan import workload_post_implementation_gates

    w = load_workload("tpch_q3_aux", REPO_ROOT)
    gates = workload_post_implementation_gates(w)
    assert "aux_correctness" in gates
    assert "aux_multi_connection" in gates
    assert "aux_bench_sweep" in gates
    assert w["engine_host"]["multi_conn_ctest_regex"] == "test_postgres_aux_multi_connection_stress"


def test_implementation_prompt_mentions_substrate_and_engine():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("tpch_q6", REPO_ROOT)
    text = render_prompt(pc, "02_implementation.md", w, 6, "implementation")
    assert "NativeScanSpec" in text or "substrate" in text.lower() or "engine_name" in text
    assert "duckdb" in text.lower()
    assert "Engine host" in text or "engine_name" in text


def test_aux_implementation_resume_uses_engine_differential():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("tpch_q3_aux", REPO_ROOT)
    msg = _implementation_user_message(pc, w, 3)
    if "RESUME" in msg:
        assert "test_postgres_tpch_differential" in msg or "postgres" in msg.lower()

    w_ddb = dict(w)
    w_ddb["engine_name"] = "duckdb"
    w_ddb["engine_host"] = engine_host_conventions("duckdb")
    from accel_pipeline.config import resolve_workload_paths

    paths = resolve_workload_paths(pc, w_ddb, 3)
    module = pc.repo_root / paths["aux_state_subdir"] / "module.cpp"
    if module.is_file():
        msg_ddb = _implementation_user_message(pc, w_ddb, 3)
        assert "test_tpch_shared_differential" in msg_ddb


def test_apply_workload_overrides_strips_duckdb_tf_for_postgres():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("tpch_q3_aux", REPO_ROOT)
    merged = apply_workload_overrides(pc, w)
    assert "backends/duckdb/api/duckdb_aux_table_functions.cpp" not in merged.write_paths
    assert not any(p.startswith("backends/") for p in merged.write_paths if "{engine}" not in p)
    assert any("postgres" in p for p in merged.write_paths)


def test_duckdb_correctness_gate_context_db_env():
    """Regression: duckdb correctness still exposes ACCEL_TPCH_DB + dataset_db."""
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("tpch_q6", REPO_ROOT)
    ctx = build_gate_context(pc, w, 6)
    assert ctx["engine_name"] == "duckdb"
    assert ctx["db_env_var"] == "ACCEL_TPCH_DB"
    assert str(ctx["dataset_db"]).endswith(".duckdb")
    assert ctx["engine_host"]["smoke_ready"] is True


def test_registration_is_per_query():
    """Queries register from their own directory; the backend names no query."""
    from accel_pipeline.config import engine_host_conventions

    for engine in ("duckdb", "postgres", "mysql"):
        host = engine_host_conventions(engine, "tpch", 7)
        for key in ("register_path", "aux_register_path"):
            assert host[key].startswith(f"accelerators/tpch/q7/{engine}/"), (engine, key, host[key])
        assert host["registry"]
