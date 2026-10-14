"""Tests for optimization source bootstrap."""

from __future__ import annotations

from pathlib import Path

from accel_pipeline.config import PipelineConfig, resolve_optimization_edit_paths
from accel_pipeline.optimization_bootstrap import (
    build_optimization_bootstrap,
    format_bootstrap_markdown,
    load_editable_sources,
)
from accel_pipeline.tools.tool_policy import STAGE_TOOL_NAMES


def test_optimization_unified_has_no_read_tools() -> None:
    tools = STAGE_TOOL_NAMES["optimization_unified"]
    assert "write_file" in tools
    assert "apply_patch" in tools
    assert "read_file" not in tools
    assert "read_reference_files" not in tools
    assert "rip_grep" in tools
    assert "search_repo" not in tools


def test_bootstrap_includes_full_sources(tmp_path: Path) -> None:
    aux = tmp_path / "accelerators/tpch/q99/duckdb/aux"
    aux.mkdir(parents=True)
    (aux / "module.hpp").write_text("#pragma once\nstruct Foo {};\n", encoding="utf-8")
    (aux / "module.cpp").write_text('#include "module.hpp"\nvoid Bar() {}\n', encoding="utf-8")
    (aux / "finalize.cpp").write_text("void Fin() {}\n", encoding="utf-8")
    tests = tmp_path / "accelerators/tpch/q99/tests"
    tests.mkdir(parents=True)
    (tests / "test_tpch_q99_aux.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")

    workload = {
        "workload_kind": "aux_state",
        "query_number": 99,
        "engine_name": "duckdb",
        "aux_state_subdir": "accelerators/tpch/q99/duckdb/aux",
        "aux_test_target": "test_tpch_q99_aux",
        "aux_bench_target": "benchmark_tpch_q99_aux",
    }
    pc = PipelineConfig(
        repo_root=tmp_path,
        pipeline_root=tmp_path / "pipeline",
        build_dir=tmp_path / "build",
        tpch_db_sf1=tmp_path / "db.duckdb",
        tpch_db_sf10=tmp_path / "db10.duckdb",
    )
    edit = resolve_optimization_edit_paths(pc, workload, 99)
    sources = load_editable_sources(tmp_path, edit)
    assert len(sources) >= 4
    assert any("module.cpp" in s["path"] for s in sources)
    assert "1: #include" in sources[1]["content"] or any(
        "#include" in s["content"] for s in sources
    )

    md, loaded = build_optimization_bootstrap(pc, workload, 99)
    assert loaded
    assert "do not call read_file" in md.lower()
    assert "Reference aux accelerators" in md
    assert "module.hpp" in md
    rendered = format_bootstrap_markdown(loaded, max_chars=500_000)
    assert "struct Foo" in rendered
