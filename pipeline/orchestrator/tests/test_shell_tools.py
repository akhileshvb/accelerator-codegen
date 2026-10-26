from __future__ import annotations

import time
from pathlib import Path

import pytest

from accel_pipeline.config import PipelineConfig
from accel_pipeline.tools.shell_tools import ToolContext, _command_allowed, _normalize_ctest_cmd, make_shell_tools


def test_normalize_ctest_cmd_adds_test_dir(tmp_path: Path) -> None:
    repo = tmp_path
    pipeline_config = PipelineConfig(
        repo_root=repo,
        pipeline_root=repo / "pipeline",
        build_dir=repo / "build-release",
        tpch_db_sf1=repo / "tpch_sf1.duckdb",
        allowed_commands=[],
    )
    assert _normalize_ctest_cmd(
        "ctest -R tpch_q21_aux --output-on-failure", pipeline_config
    ) == "ctest --test-dir build-release -R tpch_q21_aux --output-on-failure"
    assert _normalize_ctest_cmd(
        "ctest --test-dir build-release -R tpch_q21_aux", pipeline_config
    ) == "ctest --test-dir build-release -R tpch_q21_aux"


def test_builtin_diagnostic_commands_allowed_without_yaml_patterns(tmp_path: Path) -> None:
    pipeline_config = PipelineConfig(
        repo_root=tmp_path,
        pipeline_root=tmp_path / "pipeline",
        build_dir=tmp_path / "build-release",
        tpch_db_sf1=tmp_path / "tpch_sf1.duckdb",
        allowed_commands=[],
    )
    run_command = make_shell_tools(ToolContext(pipeline_config, 1))
    (tmp_path / "CMakeLists.txt").write_text("# cmake\n", encoding="utf-8")

    assert "exit=0" in run_command("pwd", timeout=5)
    assert "exit=0" in run_command("ls CMakeLists.txt", timeout=5)
    assert "exit=0" in run_command("pwd && ls CMakeLists.txt", timeout=5)

    with pytest.raises(PermissionError, match="not allowlisted"):
        run_command("cat CMakeLists.txt", timeout=5)


def test_command_allowed_helper() -> None:
    assert _command_allowed("pwd", [])
    assert _command_allowed("ls build-release", [])
    assert not _command_allowed("rm -rf build-release", [])


def test_run_command_kills_process_group_on_timeout(tmp_path: Path) -> None:
    repo = tmp_path
    (repo / "sleep.sh").write_text("#!/bin/sh\nsleep 30\n", encoding="utf-8")
    (repo / "sleep.sh").chmod(0o755)
    pipeline_config = PipelineConfig(
        repo_root=repo,
        pipeline_root=repo / "pipeline",
        build_dir=repo / "build-release",
        tpch_db_sf1=repo / "tpch_sf1.duckdb",
        allowed_commands=[r"^./sleep\.sh$"],
    )
    run_command = make_shell_tools(ToolContext(pipeline_config, 1))
    start = time.monotonic()
    out = run_command("./sleep.sh", timeout=1)
    elapsed = time.monotonic() - start
    assert "exit=124" in out
    assert "timed out" in out
    assert elapsed < 5
