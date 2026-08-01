from __future__ import annotations

import time
from pathlib import Path

from accel_pipeline.config import PipelineConfig
from accel_pipeline.tools.shell_tools import ToolContext, make_shell_tools


def test_run_command_kills_process_group_on_timeout(tmp_path: Path) -> None:
    repo = tmp_path
    (repo / "sleep.sh").write_text("#!/bin/sh\nsleep 30\n", encoding="utf-8")
    (repo / "sleep.sh").chmod(0o755)
    pipeline_config = PipelineConfig(
        repo_root=repo,
        pipeline_root=repo / "pipeline",
        build_dir=repo / "build-release",
        tpch_db_sf1=repo / "tpch_sf1.duckdb",
        tpch_db_sf10=repo / "tpch_sf10.duckdb",
        allowed_commands=[r"^./sleep\.sh$"],
    )
    run_command = make_shell_tools(ToolContext(pipeline_config, 1))
    start = time.monotonic()
    out = run_command("./sleep.sh", timeout=1)
    elapsed = time.monotonic() - start
    assert "exit=124" in out
    assert "timed out" in out
    assert elapsed < 5
