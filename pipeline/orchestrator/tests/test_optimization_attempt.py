"""Tests for optimization attempt helpers."""

from __future__ import annotations

from accel_pipeline.optimization.attempt import _parse_cannot_test, _parse_hypothesis_json


def test_parse_hypothesis_json():
    text = 'Here is my plan:\n```json\n{"bottleneck": "generic path", "why": "evidence"}\n```\nHYPOTHESIS_COMPLETE'
    payload = _parse_hypothesis_json(text)
    assert payload is not None
    assert payload["bottleneck"] == "generic path"


def test_parse_cannot_test():
    text = '```json\n{"status": "cannot_test", "reason": "need full module"}\n```\nCANNOT_TEST'
    reason = _parse_cannot_test(text)
    assert reason == "need full module"
