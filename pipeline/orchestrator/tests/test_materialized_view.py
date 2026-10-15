"""Materialized-view workload kind (Postgres SQL-only)."""

from __future__ import annotations

from pathlib import Path

import pytest

from accel_pipeline.config import (
    apply_workload_overrides,
    load_pipeline_config,
    load_workload,
    resolve_optimization_edit_paths,
    resolve_workload_paths,
)
from accel_pipeline.gate_context import build_gate_context
from accel_pipeline.host_bootstrap import needs_host_bootstrap
from accel_pipeline.prompts import render_prompt, resolve_prompt_path
from accel_pipeline.stages.gate_plan import (
    workload_post_audit_gates,
    workload_post_implementation_gates,
)


REPO_ROOT = Path(__file__).resolve().parents[3]


def test_tpch_q6_matview_loads_postgres_only():
    w = load_workload("tpch_q6_matview", REPO_ROOT)
    assert w["workload_kind"] == "materialized_view"
    assert w["engine_name"] == "postgres"
    assert w["prompt_subdir"] == "materialized_view"
    assert w["requires_base_accelerator"] is False
    assert w["engine_host"]["agent_builds_query_host"] is False
    assert "mv_apply" in w["post_implementation_gates"]
    assert any("matview" in p for p in w.get("write_paths") or [])
    assert not needs_host_bootstrap(w)


def test_matview_rejects_non_postgres():
    import yaml
    from accel_pipeline.config import _normalize_matview_workload

    raw = yaml.safe_load(
        (REPO_ROOT / "pipeline/config/workloads/tpch_q6_matview.yaml").read_text()
    )
    raw["engine_name"] = "duckdb"
    with pytest.raises(ValueError, match="postgres"):
        _normalize_matview_workload(raw, REPO_ROOT)


def test_matview_paths_and_gate_context():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("tpch_q6_matview", REPO_ROOT)
    paths = resolve_workload_paths(pc, w, 6)
    assert paths["matview_subdir"] == "eval/bench/postgres/baselines/tpch/q6/matview"
    assert paths["mv_create_sql"].endswith("create.sql")
    assert paths["mv_function_name"] == "accel_tpch_q6_mv"
    edit = resolve_optimization_edit_paths(pc, w, 6)
    assert edit["is_matview"] is True
    assert edit["is_aux"] is False
    assert any(p.endswith("create.sql") for p in edit["all_paths"])
    ctx = build_gate_context(pc, w, 6)
    assert ctx["workload_kind"] == "materialized_view"
    assert ctx["matview_subdir"] == paths["matview_subdir"]
    assert ctx["mv_relation_name"] == "accel_tpch_q6_matview"


def test_matview_gates_and_prompts():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("tpch_q6_matview", REPO_ROOT)
    assert workload_post_implementation_gates(w) == [
        "mv_apply",
        "mv_correctness",
        "mv_refresh",
        "mv_bench",
    ]
    assert "mv_bench" in workload_post_audit_gates(w)
    analysis = resolve_prompt_path(pc, w, "01_analysis.md")
    assert analysis.is_file()
    assert "materialized_view" in str(analysis)
    text = render_prompt(pc, "01_analysis.md", w, 6, "analysis")
    assert "accel_tpch_q6_mv" in text
    assert "matview" in text.lower()
    pc2 = apply_workload_overrides(pc, w)
    assert any("postgres_matview_apply" in c for c in pc2.allowed_commands)
