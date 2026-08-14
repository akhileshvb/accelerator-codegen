"""Tests for optimization tool budgets."""

from __future__ import annotations

from accel_pipeline.optimization_budget import AttemptBudget, BudgetTracker


def test_unlimited_budget_never_blocks() -> None:
    tracker = BudgetTracker(budget=AttemptBudget(max_file_reads=0, max_grep_calls=0, max_tool_calls=0))
    for _ in range(200):
        assert tracker.record("read_file") is None
        assert tracker.record("grep_context") is None
        assert tracker.record("write_file") is None
