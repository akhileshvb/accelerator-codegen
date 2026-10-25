"""Tests for random write-mix sampling + geo-mean aggregation."""

from __future__ import annotations

import random

from accel_pipeline.optimization.aux_write_freqs import (
    aggregate_write_mix_workload_speedup,
    ensure_attempt_write_frequencies,
    resolve_optimization_write_frequencies,
)


def test_resolve_static_when_sample_disabled():
    workload = {
        "aux_bench_sweep_optimization": {
            "write_frequencies": [0, 10],
            "write_mix_sample": {"enabled": False},
        }
    }
    assert resolve_optimization_write_frequencies(workload) == [0, 10]


def test_resolve_samples_per_attempt(tmp_path):
    workload = {
        "aux_bench_sweep_optimization": {
            "write_frequencies": [0],
            "write_mix_sample": {
                "enabled": True,
                "count": 2,
                "choices": [5, 15, 25, 40],
                "lock_per_attempt": True,
                "seed": 1,
            },
        }
    }
    a1 = ensure_attempt_write_frequencies(
        tmp_path, workload, optimization_iter=2, attempt=1, rng=random.Random(1)
    )
    assert a1 == resolve_optimization_write_frequencies(
        workload, art_dir=tmp_path, optimization_iter=2, attempt=1
    )
    assert a1[0] == 0
    assert len(a1) == 3
    assert set(a1[1:]).issubset({5, 15, 25, 40})

    a2 = ensure_attempt_write_frequencies(
        tmp_path, workload, optimization_iter=2, attempt=2, rng=random.Random(2)
    )
    assert a2[0] == 0
    assert len(a2) == 3
    assert a2 == resolve_optimization_write_frequencies(
        workload, art_dir=tmp_path, optimization_iter=2, attempt=2
    )
    # Seeded draws differ by attempt index.
    assert a1 != a2


def test_aggregate_write_mix_geo_mean():
    speedup, pcts = aggregate_write_mix_workload_speedup(
        {
            "by_write_frequency_pct": {
                "0": {"write_frequency_pct": 0, "workload_speedup": 9.0},
                "5": {"write_frequency_pct": 5, "workload_speedup": 4.0},
                "25": {"write_frequency_pct": 25, "workload_speedup": 1.0},
            }
        }
    )
    assert pcts == [5, 25]
    assert abs(speedup - 2.0) < 1e-9
