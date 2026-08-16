"""Smoke tests for engine_backend workloads."""

from __future__ import annotations

from pathlib import Path

from accel_pipeline.config import load_pipeline_config, load_workload, load_model_config
from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.prompts import render_prompt
from accel_pipeline.stages.engine_backend import run_engine_pipeline


REPO_ROOT = Path(__file__).resolve().parents[3]


def test_load_engine_mysql_workload():
    w = load_workload("engine_mysql", REPO_ROOT)
    assert w["workload_kind"] == "engine_backend"
    assert w["engine_name"] == "mysql"
    assert w["prompt_subdir"] == "engine_backend"


def test_engine_artifacts_dir():
    w = load_workload("engine_duckdb", REPO_ROOT)
    art = query_artifacts_dir(REPO_ROOT, 0, workload=w)
    assert "engines" in str(art)
    assert "duckdb" in str(art)


def test_render_engine_analysis_prompt():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("engine_mysql", REPO_ROOT)
    text = render_prompt(pc, "01_analysis.md", w, 0, "analysis")
    assert "NativeScanSpec" in text or "native" in text.lower()
    assert "mysql" in text.lower()
    assert "hard guidelines" in text.lower() or "Native base" in text


def test_engine_pipeline_dry_run():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    mc = load_model_config(repo_root=REPO_ROOT)
    run_engine_pipeline(pc, mc, "engine_mysql", 0, {}, dry_run=True, stage="all", skip_gates=True)
