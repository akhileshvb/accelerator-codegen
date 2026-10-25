"""Tests for optimization build/correctness fix helpers."""

from __future__ import annotations

from accel_pipeline.optimization.fix import (
    format_build_failure_diagnosis,
    format_smoke_failure_diagnosis,
    optimization_fix_limit,
)
from accel_pipeline.optimization.smoke import SmokeResult
from accel_pipeline.config import PipelineConfig
from pathlib import Path


def test_format_build_failure_includes_log_tail():
    text = format_build_failure_diagnosis(
        "error: undefined reference to Foo\nmake[2]: *** Error 1",
        optimization_iter=2,
        attempt=3,
        fix_round=1,
        reproduce_cmd="cmake --build build-release --target test",
    )
    assert "Build failure" in text
    assert "undefined reference to Foo" in text
    assert "reproduce" in text


def test_format_smoke_failure_includes_regex():
    smoke = SmokeResult(
        passed=False,
        output="Assertion `!sink.rows[0].values[0].is_null' failed",
        regex="tpch_q17_aux",
    )
    text = format_smoke_failure_diagnosis(
        smoke,
        optimization_iter=1,
        attempt=1,
        fix_round=2,
        reproduce_cmd="ctest -R tpch_q17_aux",
    )
    assert "tpch_q17_aux" in text
    assert "is_null" in text


def test_optimization_fix_limit_raises_at_cap():
    config = PipelineConfig(
        repo_root=Path("/tmp"),
        pipeline_root=Path("/tmp"),
        build_dir=Path("/tmp/build"),
        tpch_db_sf1=Path("/tmp/db"),
        tpch_db_sf10=Path("/tmp/db10"),
        optimization_build_fix_max_rounds=2,
        optimization_build_fix_safety_cap=5,
    )
    optimization_fix_limit(config, 1, kind="build")
    try:
        optimization_fix_limit(config, 2, kind="build")
        raised = False
    except RuntimeError:
        raised = True
    assert raised
