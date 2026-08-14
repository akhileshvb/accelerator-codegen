"""Legacy episodic tests — optimization is now attempt-based."""

from __future__ import annotations

from accel_pipeline.optimization_episodic import run_optimization_iteration


def test_shim_exports_run_optimization_iteration():
    assert callable(run_optimization_iteration)
