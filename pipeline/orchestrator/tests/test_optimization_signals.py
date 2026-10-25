"""Tests for optimization_signals diagnostics assembly."""

from __future__ import annotations

from accel_pipeline.optimization.signals import (
    diagnostic_deltas,
    extract_store_diagnostics,
    organize_diagnostics_evidence,
)


def test_extract_store_diagnostics_from_metrics():
    metrics = {
        "valid_partitions": 100,
        "validity_scan_us_median": 1200.5,
        "accel_hit_median_us": 50.0,
    }
    diag = extract_store_diagnostics(metrics)
    assert diag["valid_partitions"] == 100
    assert diag["validity_scan_us_median"] == 1200.5


def test_diagnostic_deltas():
    base = {"valid_partitions": 1000, "validity_scan_us_median": 500.0}
    cand = {"valid_partitions": 200, "validity_scan_us_median": 100.0}
    deltas = diagnostic_deltas(base, cand)
    assert deltas["valid_partitions"] == -800
    assert deltas["validity_scan_us_median"] == -400.0


def test_organize_diagnostics_evidence_is_factual():
    tiers = organize_diagnostics_evidence({"valid_partitions": 10, "accel_hit_rate": 1.0})
    assert "partition_store_cost" in tiers
    assert "read_path_cost" in tiers
    assert "hint" not in str(tiers).lower()
