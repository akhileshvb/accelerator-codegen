"""Tests for optimization_eval accept/reject policy."""

from __future__ import annotations

import pytest

from accel_pipeline.optimization.eval import improvement_ratio


def test_improvement_ratio():
    assert improvement_ratio(1.0, 1.05) == pytest.approx(5.0)
    assert improvement_ratio(0.0, 1.0) == 100.0


def test_aux_optimization_score_prefers_read_only():
    from accel_pipeline.optimization.eval import aux_optimization_score, score_metrics

    metrics = {
        "composite_score": 0.5,
        "composite_workload_speedup": 0.07,
        "read_only_query_speedup": 5.1,
        "speedup_median": 5.1,
    }
    workload = {"aux_bench_sweep_optimization": {"optimization_score_metric": "read_only_query_speedup"}}
    assert aux_optimization_score(metrics, workload) == pytest.approx(5.1)
    assert score_metrics(metrics, is_aux=True, workload=workload) == pytest.approx(5.1)


def test_aux_optimization_score_read_only_metric():
    from accel_pipeline.optimization.eval import aux_optimization_score

    metrics = {
        "composite_query_speedup": 6.0,
        "read_only_query_speedup": 5.0,
        "write_mix_workload_speedup": 1.0,
    }
    workload = {"aux_bench_sweep_optimization": {"optimization_score_metric": "read_only_query_speedup"}}
    assert aux_optimization_score(metrics, workload) == pytest.approx(5.0)
    assert aux_optimization_score(metrics, None) == pytest.approx(5.0**0.75)
