"""Aux optimization score fallback when DuckDB ref is skipped."""

from __future__ import annotations

from accel_pipeline.optimization_eval import aux_optimization_score, improvement_ratio


def test_aux_score_uses_hit_time_when_read_only_zero():
    slow = {"read_only_query_speedup": 0.0, "accel_hit_median_us": 20000.0}
    fast = {"read_only_query_speedup": 0.0, "accel_hit_median_us": 10000.0}
    slow_score = aux_optimization_score(slow)
    fast_score = aux_optimization_score(fast)
    assert fast_score > slow_score
    assert improvement_ratio(slow_score, fast_score) > 0


def test_aux_score_prefers_read_only_when_present():
    metrics = {"read_only_query_speedup": 9.4, "accel_hit_median_us": 1000.0}
    assert aux_optimization_score(metrics) == 9.4
