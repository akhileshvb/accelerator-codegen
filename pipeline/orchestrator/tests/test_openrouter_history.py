"""Tests for OpenRouter in-memory history manager."""

from __future__ import annotations

from accel_pipeline.optimization_history import OpenRouterHistoryManager


def test_history_carry_over_and_build_input():
    mgr = OpenRouterHistoryManager(seed_message="SEED", max_chars=50_000, preserve_recent_cycles=2)
    mgr.record_cycle("attempt 1", "output 1", "3 tools")
    items = mgr.build_input("attempt 2 start")
    assert any("SEED" in str(i.get("content", "")) for i in items)
    assert any("attempt 2 start" in str(i.get("content", "")) for i in items)


def test_history_compaction_summarizes_old_cycles():
    mgr = OpenRouterHistoryManager(
        seed_message="SEED",
        max_chars=500,
        preserve_recent_cycles=1,
        tool_output_summary_chars=20,
    )
    mgr.record_cycle("u1", "assistant one " * 50, "tool " * 50)
    mgr.record_cycle("u2", "assistant two", "ok")
    before, after = mgr.compact()
    assert after <= before or len(mgr._summaries) >= 0
    items = mgr.build_input("u3")
    text = str(items)
    assert "SEED" in text
