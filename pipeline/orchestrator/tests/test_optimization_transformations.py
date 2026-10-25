"""Tests for transformation retrieval."""

from __future__ import annotations

from pathlib import Path

from accel_pipeline.optimization.transformations import (
    load_transformation_catalog,
    retrieve_transformations,
)


def test_catalog_loads(repo_root: Path | None = None):
    root = repo_root or Path(__file__).resolve().parents[3]
    catalog = load_transformation_catalog(root)
    assert len(catalog) >= 2


def test_retrieve_ranks_high_validity_scan(repo_root: Path | None = None):
    root = repo_root or Path(__file__).resolve().parents[3]
    precedents = retrieve_transformations(
        root,
        store_diagnostics={"validity_slots_scanned_median": 200000, "valid_partitions": 200000},
        analysis={"likely_bottlenecks": ["string bind param"]},
        limit=2,
    )
    assert precedents
    assert "problem" in precedents[0]
    assert "apply this pattern" not in str(precedents).lower()
