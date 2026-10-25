"""Tests for optimization_evidence extraction."""

from __future__ import annotations

from accel_pipeline.optimization.evidence import extract_bottleneck_evidence


def test_extract_evidence_amdahl_upper_bound():
    diagnosis = {
        "profile_phases_us": {"pre_consume_us": 800.0, "num_scan_us": 200.0},
        "profile_counters": {},
        "dominant_phase": "pre_consume_us",
        "dominant_phase_us": 800.0,
        "profile_signals": [],
        "findings": [],
        "lacking": [],
    }
    evidence = extract_bottleneck_evidence(diagnosis, {}, None)
    tiers = evidence["tiers"]
    assert tiers
    has_bound = any(t.get("upper_bound_pct") == 80.0 for t in tiers)
    assert has_bound


def test_no_speculative_upper_bound_on_generic_rows():
    diagnosis = {
        "profile_phases_us": {},
        "profile_counters": {"generic_rows": 1000, "pre_rows": 2000},
        "profile_signals": [],
        "findings": [],
        "lacking": [],
    }
    evidence = extract_bottleneck_evidence(diagnosis, {}, None)
    for tier in evidence["tiers"]:
        assert "upper_bound_pct" not in tier or tier.get("relevant_phases_us")
