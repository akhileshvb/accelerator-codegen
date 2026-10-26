"""Tests for agent OPTIMIZATION_SATURATED early-stop helpers."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

from accel_pipeline.optimization_saturation import Verdict, judge_saturation_claim, parse_optimization_saturated


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
    verdict, detail, _ = judge_saturation_claim(cfg, art, {"reason": "stuck around 16x; no further dual-axis movement"})
    assert verdict is not Verdict.REJECT
    assert "16x" in detail or "stuck" in detail


def test_evaluate_respects_disabled(tmp_path: Path):
    art = tmp_path / "art"
    art.mkdir()
    cfg = SimpleNamespace(optimization_early_stop={"enabled": False})
    verdict, detail, _ = judge_saturation_claim(cfg, art, {"reason": "stuck"})
    assert verdict is Verdict.REJECT
    assert "disabled" in detail
