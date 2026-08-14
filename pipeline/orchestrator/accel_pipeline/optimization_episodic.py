"""Backward-compatible shim — optimization now uses optimization_attempt."""

from accel_pipeline.optimization_attempt import run_optimization_iteration

__all__ = ["run_optimization_iteration"]
