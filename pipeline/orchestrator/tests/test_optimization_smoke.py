"""Tests for optimization smoke helpers."""

from __future__ import annotations

from unittest.mock import patch

from accel_pipeline.optimization_smoke import SmokeResult, run_smoke_correctness_repeated


@patch("accel_pipeline.optimization_smoke.run_smoke_correctness")
def test_smoke_repeated_all_pass(mock_run):
    mock_run.side_effect = [
        SmokeResult(passed=True, output="ok1", regex="tpch_q17_aux"),
        SmokeResult(passed=True, output="ok2", regex="tpch_q17_aux"),
    ]
    result = run_smoke_correctness_repeated(None, {}, 17, runs=2)
    assert result.passed
    assert mock_run.call_count == 2


@patch("accel_pipeline.optimization_smoke.run_smoke_correctness")
def test_smoke_repeated_stops_on_first_fail(mock_run):
    mock_run.side_effect = [
        SmokeResult(passed=True, output="ok1", regex="tpch_q17_aux"),
        SmokeResult(passed=False, output="fail", regex="tpch_q17_aux"),
    ]
    result = run_smoke_correctness_repeated(None, {}, 17, runs=2)
    assert not result.passed
    assert mock_run.call_count == 2
