"""Smoke tests for engine_backend workloads."""

from __future__ import annotations

from pathlib import Path

from accel_pipeline.config import (
    apply_workload_overrides,
    load_pipeline_config,
    load_workload,
    load_model_config,
    sanitize_engine_name,
)
from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.prompts import render_prompt
from accel_pipeline.stages.engine_backend import run_engine_pipeline
from accel_pipeline.write_ownership import write_allowed


REPO_ROOT = Path(__file__).resolve().parents[3]


def test_load_engine_mysql_workload():
    w = load_workload("engine_mysql", REPO_ROOT)
    assert w["workload_kind"] == "engine_backend"
    assert w["engine_name"] == "mysql"
    assert w["prompt_subdir"] == "engine_backend"
    assert w["target_paths"]["scan_adapter_header"].endswith("runtime/mysql/scan_adapter.hpp")
    assert w["cmake"]["collect_fn"] == "accel_collect_mysql_sources"
    assert w["cmake"]["build_flag"] == "ACCELERATOR_CODEGEN_BUILD_MYSQL"
    assert any("runtime/mysql/**" in p for p in w["write_paths"])
    assert any("stateful/mysql/**" in p for p in w["write_paths"])


def test_engine_duckdb_preserves_historical_paths():
    w = load_workload("engine_duckdb", REPO_ROOT)
    assert "duckdb_scan_adapter.hpp" in w["target_paths"]["scan_adapter_header"]
    assert w["cmake"]["collect_fn"] == "accel_collect_stateful_sources"
    assert w["cmake"]["build_flag"] == ""


def test_engine_write_paths_allow_adapter_edits():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("engine_mysql", REPO_ROOT)
    merged = apply_workload_overrides(pc, w)
    ok, reason = write_allowed(
        REPO_ROOT,
        "include/accelerator_codegen/runtime/mysql/scan_adapter.hpp",
        query=0,
        stage="implementation",
        write_paths=merged.write_paths,
        pipeline_config=merged,
        benchmark="engine",
    )
    assert ok, reason
    ok2, reason2 = write_allowed(
        REPO_ROOT,
        "src/stateful/mysql/mysql_aux_backend.cpp",
        query=0,
        stage="implementation",
        write_paths=merged.write_paths,
        pipeline_config=merged,
        benchmark="engine",
    )
    assert ok2, reason2


def test_sanitize_engine_name():
    assert sanitize_engine_name("MySQL") == "mysql"
    assert sanitize_engine_name("my-sql") == "my_sql"


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
    assert "runtime/mysql" in text
    assert "engine-agnostic" in text.lower() or "not require sibling" in text.lower()


def test_engine_pipeline_dry_run():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    mc = load_model_config(repo_root=REPO_ROOT)
    run_engine_pipeline(pc, mc, "engine_mysql", 0, {}, dry_run=True, stage="all", skip_gates=True)
