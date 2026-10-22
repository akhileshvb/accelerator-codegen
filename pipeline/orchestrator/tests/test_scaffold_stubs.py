from __future__ import annotations

from pathlib import Path

import pytest

from accel_pipeline.config import load_pipeline_config, load_workload
from accel_pipeline.workload_facts import with_facts
from accel_pipeline.scaffold_stubs import _build_stub_values, _type_prefix, materialize_workload_stubs


def test_type_prefix_from_create_fn():
    assert _type_prefix("CreateSecEdgarQ1FunctionSet") == "SecEdgarQ1"
    assert _type_prefix("CreateTpchQ3FunctionSet") == "TpchQ3"


def test_build_stub_values_sec_edgar():
    repo = Path(__file__).resolve().parents[3]
    pipeline_config = load_pipeline_config(repo_root=repo)
    workload = load_workload("sec_edgar_q1", repo)
    paths = {
        "function_name": "accel_sec_edgar_q1",
        "create_fn": "CreateSecEdgarQ1FunctionSet",
        "test_target": "test_sec_edgar_q1",
        "bench_target": "benchmark_sec_edgar_q1",
        "header_path": "accelerators/sec_edgar/q1/duckdb/base/query.hpp",
    }
    values = _build_stub_values(with_facts(repo, workload, 1), paths, 1)
    assert values["type_prefix"] == "SecEdgarQ1"
    assert values["scan_spec_fn"] == "CreateSecEdgarQ1ScanSpec"
    assert '"stmt"' in values["output_schema_names"]


def test_materialize_workload_stubs_writes_missing_files(tmp_path):
    repo = tmp_path / "repo"
    pipe = repo / "pipeline"
    (pipe / "templates" / "scaffold").mkdir(parents=True)
    (pipe / "templates" / "sec_edgar").mkdir(parents=True)
    (pipe / "config" / "benchmark_packs").mkdir(parents=True)
    (pipe / "config" / "workloads").mkdir(parents=True)

    # minimal templates
    (pipe / "templates" / "scaffold" / "header.hpp.tpl").write_text(
        "#pragma once\n// {{type_prefix}} {{scan_spec_fn}}\n",
        encoding="utf-8",
    )
    (pipe / "templates" / "scaffold" / "kernel.cpp.tpl").write_text(
        "// {{kernel_fn}}\n",
        encoding="utf-8",
    )
    (pipe / "templates" / "scaffold" / "package.cpp.tpl").write_text(
        "// {{create_fn}}\n",
        encoding="utf-8",
    )

    (repo / "workloads" / "sec_edgar").mkdir(parents=True)
    (repo / "workloads" / "sec_edgar" / "schema.sql").write_text("CREATE TABLE sub (adsh VARCHAR, fy INTEGER);\n", encoding="utf-8")
    sql = repo / "workloads" / "sec_edgar" / "q1.sql"
    sql.write_text("SELECT adsh FROM sub WHERE fy = 2022\n", encoding="utf-8")

    workload = {
        "reference_sql_path": str(sql),
        "benchmark_name": "sec_edgar",
        "engine_name": "duckdb",
        "query_number": 1,
        "artifact_namespace": "sec_edgar",
        "source_subdir": "sec_edgar/q1/base/single-threaded",
        "function_name": "accel_sec_edgar_q1",
        "create_fn": "CreateSecEdgarQ1FunctionSet",
        "header_basename": "sec_edgar_q1",
        "test_target": "test_sec_edgar_q1",
        "bench_target": "benchmark_sec_edgar_q1",
    }

    from accel_pipeline.config import PipelineConfig

    pipeline_config = PipelineConfig(
        repo_root=repo,
        pipeline_root=pipe,
        build_dir=repo / "build-release",
        tpch_db_sf1=repo / "tpch_sf1.duckdb",
        tpch_db_sf10=repo / "tpch_sf10.duckdb",
    )

    result = materialize_workload_stubs(pipeline_config, workload, 1)
    header = repo / "accelerators/sec_edgar/q1/duckdb/base/query.hpp"
    assert header.is_file()
    assert "SecEdgarQ1" in header.read_text(encoding="utf-8")
    assert "query.hpp" in result["created"][0]

