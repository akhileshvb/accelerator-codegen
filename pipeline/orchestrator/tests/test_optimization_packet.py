"""Tests for optimization packet size budget."""

from __future__ import annotations

from accel_pipeline.optimization.packet import CONTEXT_MAX_CHARS, CONTEXT_MAX_LINES, format_context_markdown


def test_context_packet_hard_caps():
    huge = {
        "baseline_score": 1.0,
        "summary": "x\n" * 500,
        "hot_path_code": "y\n" * 500,
        "api_signatures": "z\n" * 200,
        "measurements": "{}",
        "evidence": "{}",
        "failed_hypotheses": [],
        "kernel_paths": {},
    }
    rendered = format_context_markdown(huge)
    assert len(rendered) <= CONTEXT_MAX_CHARS
    assert len(rendered.splitlines()) <= CONTEXT_MAX_LINES
