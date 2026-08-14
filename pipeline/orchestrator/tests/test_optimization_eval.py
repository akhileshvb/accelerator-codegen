"""Tests for optimization_eval accept/reject policy."""

from __future__ import annotations

import pytest

from accel_pipeline.optimization_eval import (
    EvalConfig,
    evaluate_confirmation,
    evaluate_initial_screen,
    improvement_ratio,
)


def test_improvement_ratio():
    assert improvement_ratio(1.0, 1.05) == pytest.approx(5.0)
    assert improvement_ratio(0.0, 1.0) == 100.0


def test_initial_screen_rejects_below_2pct():
    cfg = EvalConfig(reject_threshold_pct=2.0)
    result = evaluate_initial_screen(1.0, 1.01, cfg)
    assert result.verdict == "reject"


def test_initial_screen_needs_confirmation_at_2pct():
    cfg = EvalConfig(reject_threshold_pct=2.0)
    result = evaluate_initial_screen(1.0, 1.025, cfg)
    assert result.verdict == "needs_confirmation"


def test_confirmation_terminal_accept():
    cfg = EvalConfig(improvement_threshold_pct=3.0)
    ok = evaluate_confirmation(1.0, 1.04, cfg)
    assert ok.accepted is True


def test_confirmation_terminal_reject():
    cfg = EvalConfig(improvement_threshold_pct=3.0)
    bad = evaluate_confirmation(1.0, 1.025, cfg)
    assert bad.accepted is False
    assert "confirmation" in bad.reason


def test_aux_optimization_score_prefers_read_only():
    from accel_pipeline.optimization_eval import aux_optimization_score, score_metrics

    metrics = {
        "composite_score": 0.5,
        "composite_workload_speedup": 0.07,
        "read_only_query_speedup": 5.1,
        "speedup_median": 5.1,
    }
    workload = {"aux_bench_sweep_optimization": {"optimization_score_metric": "read_only_query_speedup"}}
    assert aux_optimization_score(metrics, workload) == pytest.approx(5.1)
    assert score_metrics(metrics, is_aux=True, workload=workload) == pytest.approx(5.1)


def test_aux_optimization_score_custom_metric():
    from accel_pipeline.optimization_eval import aux_optimization_score

    metrics = {"composite_query_speedup": 6.0, "read_only_query_speedup": 5.0}
    workload = {"aux_bench_sweep_optimization": {"optimization_score_metric": "composite_query_speedup"}}
    assert aux_optimization_score(metrics, workload) == pytest.approx(6.0)
