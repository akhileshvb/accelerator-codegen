"""Aux optimization score: dual-axis blend + hit-time fallback."""

from __future__ import annotations

from accel_pipeline.optimization.eval import aux_optimization_score, improvement_ratio


def test_aux_score_uses_hit_time_when_read_only_zero():
    slow = {"read_only_query_speedup": 0.0, "accel_hit_median_us": 20000.0}
    fast = {"read_only_query_speedup": 0.0, "accel_hit_median_us": 10000.0}
    slow_score = aux_optimization_score(slow)
    fast_score = aux_optimization_score(fast)
    assert fast_score > slow_score
    assert improvement_ratio(slow_score, fast_score) > 0


def test_aux_score_read_only_when_no_write_mix():
    metrics = {"read_only_query_speedup": 9.4, "accel_hit_median_us": 1000.0}
    assert aux_optimization_score(metrics) == 9.4


def test_aux_score_dual_axis_weights_write_less_than_read():
    base = {
        "read_only_query_speedup": 9000.0,
        "write_mix_workload_speedup": 0.37,
    }
    base_score = aux_optimization_score(base)

    # Same write-mix, modest read gain — positive but limited.
    read_gain = aux_optimization_score(
        {"read_only_query_speedup": 10000.0, "write_mix_workload_speedup": 0.37}
    )
    read_imp = improvement_ratio(base_score, read_gain)

    # Same read, write-mix 0.37 → 2.0 — larger dual-axis lift than the read-only bump.
    write_gain = aux_optimization_score(
        {"read_only_query_speedup": 9000.0, "write_mix_workload_speedup": 2.0}
    )
    write_imp = improvement_ratio(base_score, write_gain)
    assert write_imp > read_imp

    # Read up but write cliff deepens — net regress.
    cliff = aux_optimization_score(
        {"read_only_query_speedup": 9500.0, "write_mix_workload_speedup": 0.15}
    )
    assert improvement_ratio(base_score, cliff) < 0


def test_aux_score_respects_explicit_read_only_metric():
    workload = {
        "aux_bench_sweep_optimization": {
            "optimization_score_metric": "read_only_query_speedup",
        }
    }
    metrics = {
        "read_only_query_speedup": 12.0,
        "write_mix_workload_speedup": 0.1,
    }
    assert aux_optimization_score(metrics, workload) == 12.0


def test_aux_score_reads_write_mix_from_by_freq():
    metrics = {
        "read_only_query_speedup": 100.0,
        "by_write_frequency_pct": {"10": {"workload_speedup": 4.0}},
    }
    expected = (100.0**0.75) * (4.0**0.25)
    assert abs(aux_optimization_score(metrics) - expected) < 1e-6


def test_aux_score_geo_means_multiple_write_mixes():
    metrics = {
        "read_only_query_speedup": 16.0,
        "by_write_frequency_pct": {
            "0": {"write_frequency_pct": 0, "workload_speedup": 20.0},
            "10": {"write_frequency_pct": 10, "workload_speedup": 4.0},
            "50": {"write_frequency_pct": 50, "workload_speedup": 1.0},
        },
    }
    write_mix = (4.0 * 1.0) ** 0.5
    expected = (16.0**0.75) * (write_mix**0.25)
    assert abs(aux_optimization_score(metrics) - expected) < 1e-6
