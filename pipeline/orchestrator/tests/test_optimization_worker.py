"""Tests for optimization worker markers and cycle parsing."""

from __future__ import annotations

from accel_pipeline.optimization_worker import (
    CANDIDATE_READY,
    _parse_cannot_test,
    _parse_hypothesis_json,
)


def test_parse_hypothesis_json_candidate_ready():
    text = (
        'Plan:\n```json\n{"bottleneck": "decode", "why": "evidence"}\n```\n'
        f"{CANDIDATE_READY}"
    )
    payload = _parse_hypothesis_json(text)
    assert payload is not None
    assert payload["bottleneck"] == "decode"


def test_parse_cannot_test_worker():
    text = '```json\n{"status": "cannot_test", "reason": "blocked"}\n```\nCANNOT_TEST'
    assert _parse_cannot_test(text) == "blocked"
