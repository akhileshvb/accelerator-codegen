"""Tests for PendingFeedback queue."""

from __future__ import annotations

from accel_pipeline.optimization.pending import PendingFeedback, format_gate_delta


def test_pending_feedback_flush_clears():
    pending = PendingFeedback()
    pending.push("Previous candidate rejected.")
    msg = pending.flush_into_attempt_prompt(optimization_iter=1, attempt=2)
    assert "Previous candidate rejected" in msg
    assert pending.peek() == ""


def test_format_gate_delta_reverted():
    text = format_gate_delta(
        verdict="reject_regress",
        reason="too slow",
        hypothesis={"bottleneck": "foo"},
        improvement_pct=-1.5,
        reverted=True,
    )
    assert "reject_regress" in text
    assert "reverted" in text.lower()
