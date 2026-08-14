from __future__ import annotations

from pathlib import Path

import pytest

from accel_pipeline.config import PipelineConfig
from accel_pipeline.tools.repo_tools import ToolContext, make_repo_tools


@pytest.fixture
def repo_tools(tmp_path: Path):
    repo = tmp_path / "repo"
    repo.mkdir()
    pipeline_config = PipelineConfig(
        repo_root=repo,
        pipeline_root=repo / "pipeline",
        build_dir=repo / "build-release",
        tpch_db_sf1=repo / "tpch.duckdb",
        tpch_db_sf10=repo / "tpch10.duckdb",
        write_paths=[],
    )
    tools = make_repo_tools(ToolContext(pipeline_config, 1))
    read_file, grep_context = tools[0], tools[8]
    return repo, read_file, grep_context


def test_grep_context_returns_matching_lines_with_context(repo_tools):
    repo, _read_file, grep_context = repo_tools
    target = repo / "sample.cpp"
    target.write_text(
        "\n".join(
            [
                "line one",
                "void ConsumeRow() {",
                "  work();",
                "}",
                "line five",
            ]
        ),
        encoding="utf-8",
    )
    output = grep_context("sample.cpp", "ConsumeRow", context=1)
    assert "ConsumeRow" in output
    assert "work();" in output
    assert ">" in output


def test_grep_context_no_matches(repo_tools):
    repo, _read_file, grep_context = repo_tools
    target = repo / "empty.txt"
    target.write_text("hello\n", encoding="utf-8")
    output = grep_context("empty.txt", "missing_symbol")
    assert "(no matches)" in output
