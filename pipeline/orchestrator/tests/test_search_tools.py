"""Tests for rip_grep and ast_grep search tools."""

from __future__ import annotations

from pathlib import Path

from accel_pipeline.config import PipelineConfig
from accel_pipeline.tools.search_tools import make_search_tools


def _pc(tmp_path: Path) -> PipelineConfig:
    sample = tmp_path / "src/generated/tpch/q18/aux"
    sample.mkdir(parents=True)
    (sample / "module.cpp").write_text(
        "void NationNamePartition() {}\nvoid Other() {}\n",
        encoding="utf-8",
    )
    return PipelineConfig(
        repo_root=tmp_path,
        pipeline_root=tmp_path / "pipeline",
        build_dir=tmp_path / "build",
        tpch_db_sf1=tmp_path / "db.duckdb",
        tpch_db_sf10=tmp_path / "db10.duckdb",
    )


def test_rip_grep_finds_pattern_with_context(tmp_path: Path) -> None:
    rip_grep, _ = make_search_tools(_pc(tmp_path))
    out = rip_grep("NationNamePartition", path="src/generated/tpch/q18/aux", context=1)
    assert "NationNamePartition" in out
    assert "no matches" not in out


def test_rip_grep_no_matches(tmp_path: Path) -> None:
    rip_grep, _ = make_search_tools(_pc(tmp_path))
    out = rip_grep("MissingSymbolXYZ", path=".")
    assert "no matches" in out


def test_ast_grep_graceful_when_missing(tmp_path: Path) -> None:
    _, ast_grep = make_search_tools(_pc(tmp_path))
    out = ast_grep("class $C { $$$ }", path="src/generated/tpch/q18/aux")
    assert "unavailable" in out.lower() or "no ast matches" in out.lower() or "{" in out
