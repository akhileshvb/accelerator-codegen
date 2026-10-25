"""Tests for agent OPTIMIZATION_SATURATED early-stop helpers."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

from accel_pipeline.optimization.saturation import (
    evaluate_saturation_claim,
    parse_optimization_saturated,
)


def test_parse_optimization_saturated_with_json():
    text = """
Done optimizing for now.

OPTIMIZATION_SATURATED

```json
{"status": "optimization_saturated", "reason": "stuck after aggressive pass", "remaining_headroom": "low"}
```
"""
    payload = parse_optimization_saturated(text)
    assert payload is not None
    assert payload["status"] == "optimization_saturated"
    assert "stuck" in payload["reason"]


def test_parse_optimization_saturated_absent():
    assert parse_optimization_saturated("CANDIDATE_READY") is None


def test_evaluate_honors_agent_without_numeric_floors(tmp_path: Path):
    art = tmp_path / "art"
    art.mkdir()
    cfg = SimpleNamespace(optimization_early_stop={"enabled": True})
    ok, detail, _ = evaluate_saturation_claim(
        cfg,
        {"workload_kind": "aux_state", "name": "tpch_q16_aux"},
        art,
        {"reason": "stuck around 16x; no further dual-axis movement"},
    )
    assert ok
    assert "16x" in detail or "stuck" in detail


def test_evaluate_respects_disabled(tmp_path: Path):
    art = tmp_path / "art"
    art.mkdir()
    cfg = SimpleNamespace(optimization_early_stop={"enabled": False})
    ok, detail, _ = evaluate_saturation_claim(
        cfg,
        {"workload_kind": "aux_state"},
        art,
        {"reason": "stuck"},
    )
    assert not ok
    assert "disabled" in detail
