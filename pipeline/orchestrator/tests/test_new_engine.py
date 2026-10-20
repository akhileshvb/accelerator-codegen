"""A new engine is a profile (plus its backend's host.sh and api/README.md): no code change.

`toydb` exists only as a fixture. The tree below is a copy of the pipeline's config, schemas,
prompts and templates with the fixture engine added and a tpch aux workload opened to it.
"""

from __future__ import annotations

import json
import shutil
from pathlib import Path

import pytest

from accel_pipeline.config import apply_workload_overrides, load_pipeline_config, load_workload
from accel_pipeline.engine_host import host_info
from accel_pipeline.gate_context import build_gate_context
from accel_pipeline.prompts import render_prompt
from accel_pipeline.run_pack import parse_pack_queries
from accel_pipeline.scaffold_stubs import materialize_workload_stubs, read_api_manifest

REAL_PIPELINE = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).parent / "fixtures"
WORKLOAD = "tpch_q1_aux"


@pytest.fixture()
def repo(tmp_path: Path) -> Path:
    pipeline = tmp_path / "pipeline"
    for sub in ("config", "schemas", "prompts", "templates"):
        shutil.copytree(REAL_PIPELINE / sub, pipeline / sub)
    shutil.copytree(REAL_PIPELINE.parent / "workloads", tmp_path / "workloads")
    shutil.copy(FIXTURES / "engine_toydb.yaml", pipeline / "config" / "workloads")
    shutil.copytree(FIXTURES / "toydb" / "backends", tmp_path / "backends")
    workload = pipeline / "config" / "workloads" / f"{WORKLOAD}.yaml"
    workload.write_text(
        workload.read_text(encoding="utf-8").replace("engines: [duckdb, postgres, mysql]", "engines: [duckdb, postgres, mysql, toydb]"),
        encoding="utf-8",
    )
    return tmp_path


def test_toydb_aux_run_loads_with_engine_derived_host(repo: Path):
    w = load_workload(WORKLOAD, repo, engine="toydb")
    host = w["engine_host"]
    assert w["engine_name"] == "toydb"
    assert host["register_path"] == "accelerators/tpch/q1/toydb/register.cpp"
    assert host["agent_builds_query_host"] is True
    assert host["differential_binary"] == "test_toydb_tpch_differential"
    assert host["work_db_suffix"] == ".toydb_work"
    assert all("/q{N}/toydb/" in p for p in w["fairness"]["kernel_search_paths"])
    merged = apply_workload_overrides(load_pipeline_config(repo_root=repo), w)
    assert host["differential_source"] in merged.write_paths


def test_toydb_prompts_and_gate_context_render(repo: Path):
    pc = load_pipeline_config(repo_root=repo)
    w = load_workload(WORKLOAD, repo, engine="toydb")
    for prompt, stage in (("01_analysis.md", "analysis"), ("02_implementation.md", "implementation")):
        text = render_prompt(pc, prompt, w, 1, stage)
        assert "toydb" in text and "{{" not in text
    ctx = build_gate_context(pc, w, 1)
    assert ctx["engine_name"] == "toydb"
    assert ctx["fairness"]["kernel_search_paths"] == w["fairness"]["kernel_search_paths"]


def test_toydb_host_sh_and_scaffold_follow_the_backends_files(repo: Path):
    pc = load_pipeline_config(repo_root=repo)
    assert host_info(repo, "toydb", repo / "build")["embedded"] is True
    manifest = read_api_manifest(repo, "toydb")
    assert manifest["registration"]["symbols"] == ["ToyRegisterQuery", "ToyQueryRegistrar"]
    assert manifest["scan"]["headers"] == ["backends/toydb/api/scan.hpp", "backends/toydb/api/rows.hpp"]

    w = load_workload(WORKLOAD, repo, engine="toydb")
    created = materialize_workload_stubs(pc, w, 1)["created"]
    register = repo / "accelerators/tpch/q1/toydb/register.cpp"
    assert str(register.relative_to(repo)) in created
    text = register.read_text(encoding="utf-8")
    assert '#include "backends/toydb/api/register.hpp"' in text and "ToyQueryRegistrar" in text
    assert "duckdb" not in text.lower() and "{{" not in text


def test_pack_expands_over_toydb(repo: Path):
    pack = {"engines": ["toydb"], "workload_template": "tpch_q{q}_aux", "queries": [{"query": 1}]}
    (entry,) = parse_pack_queries(pack)
    assert (entry.workload, entry.engine) == (WORKLOAD, "toydb")
    assert load_workload(entry.workload, repo, engine=entry.engine)["engine_name"] == "toydb"
