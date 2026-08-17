"""Tests for post-analysis adversarial_tests stage validation."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from accel_pipeline.stages.adversarial_tests import validate_scenarios


def _valid(query: int = 7) -> dict:
    return {
        "version": 1,
        "query": query,
        "point_writes": {
            "min_count": 4,
            "max_probes": 8,
            "tables": ["orders", "lineitem"],
            "notes": "point updates on join keys",
        },
        "bulk_writes": {
            "required": True,
            "min_chunks": 2,
            "notes": "RF1/RF2",
        },
        "rationale": "Q7 joins orders/lineitem; RF and multi-point probes must invalidate.",
    }


def test_validate_scenarios_ok(tmp_path: Path) -> None:
    path = tmp_path / "adversarial_write_scenarios.json"
    path.write_text(json.dumps(_valid()), encoding="utf-8")
    validate_scenarios(path, query=7)


def test_validate_scenarios_rejects_weak_point_coverage(tmp_path: Path) -> None:
    data = _valid()
    data["point_writes"]["min_count"] = 2
    path = tmp_path / "adversarial_write_scenarios.json"
    path.write_text(json.dumps(data), encoding="utf-8")
    with pytest.raises(RuntimeError, match="min_count"):
        validate_scenarios(path, query=7)


def test_validate_scenarios_rejects_bulk_required_without_chunks(tmp_path: Path) -> None:
    data = _valid()
    data["bulk_writes"]["min_chunks"] = 0
    path = tmp_path / "adversarial_write_scenarios.json"
    path.write_text(json.dumps(data), encoding="utf-8")
    with pytest.raises(RuntimeError, match="min_chunks"):
        validate_scenarios(path, query=7)
