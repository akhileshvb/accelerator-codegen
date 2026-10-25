"""Tests for shared optimization negative memory."""

from __future__ import annotations

from pathlib import Path

from accel_pipeline.optimization.negative_memory import (
    append_negative_memory,
    ensure_negative_memory,
    format_negative_memory_markdown,
    load_negative_memory,
    maybe_record_reject_trap,
)


def test_ensure_seeds_and_dedupes(tmp_path: Path, monkeypatch):
    repo = tmp_path / "repo"
    (repo / "pipeline").mkdir(parents=True)

    # Point pipeline_root via monkeypatch on negative_memory.pipeline_root
    import accel_pipeline.optimization.negative_memory as nm

    monkeypatch.setattr(nm, "pipeline_root", lambda root: root / "pipeline")

    path = ensure_negative_memory(repo, "duckdb")
    assert path.is_file()
    seeded = load_negative_memory(repo, "duckdb", limit=20)
    assert len(seeded) >= 4

    first = append_negative_memory(
        repo,
        "duckdb",
        trap="Do not leave MarkRebuild as the only point-write path",
        when="aux maintenance",
        source="agent",
        query=5,
    )
    assert first is not None
    again = append_negative_memory(
        repo,
        "duckdb",
        trap="Do not leave MarkRebuild as the only point-write path",
        when="aux maintenance",
        source="agent",
        query=5,
    )
    assert again is None

    mild = maybe_record_reject_trap(
        repo,
        query=5,
        workload={"workload_kind": "aux_state", "engine_name": "duckdb"},
        hypothesis={"bottleneck": "tiny read shuffle"},
        verdict="reject_regress",
        reason="below_threshold",
        improvement_pct=-2.0,
    )
    assert mild is None

    hard = maybe_record_reject_trap(
        repo,
        query=5,
        workload={"workload_kind": "aux_state", "engine_name": "duckdb"},
        hypothesis={"bottleneck": "hit-path hashmap materialize", "why": "hit 120ms"},
        verdict="reject_regress",
        reason="dual_axis_score_severely_regressed",
        improvement_pct=-40.0,
    )
    assert hard is not None

    md = format_negative_memory_markdown(load_negative_memory(repo, "duckdb", limit=8), limit=8)
    assert md and md[0].startswith("## Negative memory")
    assert "advisory" in md[0].lower() or "advisory" in md[1].lower()


def test_traps_are_per_engine(tmp_path: Path, monkeypatch):
    repo = tmp_path / "repo"
    (repo / "pipeline").mkdir(parents=True)
    import accel_pipeline.optimization.negative_memory as nm

    monkeypatch.setattr(nm, "pipeline_root", lambda root: root / "pipeline")
    trap = "Do not rebuild the whole hash table on every point write"
    assert append_negative_memory(repo, "postgres", trap=trap, when="aux maintenance", source="agent", query=1)
    assert trap in {e["trap"] for e in load_negative_memory(repo, "postgres", limit=50)}
    assert trap not in {e["trap"] for e in load_negative_memory(repo, "mysql", limit=50)}
