"""Tests for stable scoreboard + custom profile aggregation."""

from __future__ import annotations

import json
import time
from pathlib import Path

from accel_pipeline.optimization.aux_profile import build_aux_profile_digest, write_aux_profile_artifacts
from accel_pipeline.optimization.scoreboard import (
    aggregate_custom_profile_events,
    build_scoreboard,
    primary_score_from_metrics,
    profile_freshness,
    write_scoreboard,
)


def test_aggregate_custom_profile_split_timers_and_counts():
    events = [
        {"phase": "sort", "us": 100.0, "emitted_rows": 200, "groups": 200},
        {"phase": "sort", "us": 120.0, "emitted_rows": 220, "groups": 220},
        {"phase": "count", "us": 40.0, "rows": 1000},
        {"aggregate_us": 15.0, "groups": 12},
    ]
    agg = aggregate_custom_profile_events(events)
    assert agg["dominant_phase"] == "sort"
    assert agg["event_count"] == 4
    phases = {row["phase"]: row for row in agg["phases"]}
    assert phases["sort"]["n"] == 2
    assert phases["sort"]["mean_us"] == 110.0
    assert phases["sort"]["counts_mean"]["emitted_rows"] == 210.0
    assert "rows" in agg["work_counters_mean"]
    assert agg["timed_phases_mean_sum_us"] > 0


def test_scoreboard_prefers_best_valid(tmp_path: Path):
    (tmp_path / "best_valid_baseline.json").write_text(
        json.dumps(
            {
                "attempt_id": "iter1_a1",
                "score": 4.405,
                "metrics": {
                    "read_only_query_speedup": 4.4,
                    "accel_hit_median_us": 9287.64,
                },
            }
        ),
        encoding="utf-8",
    )
    (tmp_path / "profile_summary.json").write_text(
        json.dumps(
            {
                "read_only_query_speedup": 2.0,
                "accel_hit_median_us": 20000.0,
            }
        ),
        encoding="utf-8",
    )
    board = build_scoreboard(tmp_path, is_aux=True)
    write_scoreboard(tmp_path, board)
    assert board["compare_against"] == "best_valid_score"
    # Recomputed from metrics (no write-mix → read_only alone).
    assert board["best_valid_score"] == 4.4
    assert board["score_metric"].startswith("dual_axis_weighted")
    assert board["best_valid_attempt_id"] == "iter1_a1"
    assert board["last_measured_score"] == 2.0
    assert board["improvement_pct_vs_best_valid"] < 0
    assert (tmp_path / "optimization_scoreboard.json").is_file()


def test_scoreboard_dual_axis_blends_write_mix(tmp_path: Path):
    (tmp_path / "best_valid_baseline.json").write_text(
        json.dumps(
            {
                "attempt_id": "iter1_a2",
                "score": 9000.0,  # stale absolute score; must recompute
                "metrics": {
                    "read_only_query_speedup": 9000.0,
                    "write_mix_workload_speedup": 0.37,
                    "accel_hit_median_us": 8.0,
                },
            }
        ),
        encoding="utf-8",
    )
    board = build_scoreboard(tmp_path, is_aux=True)
    expected = (9000.0**0.75) * (0.37**0.25)
    assert abs(float(board["best_valid_score"]) - expected) < 1e-6
    assert float(board["best_valid_score"]) < 9000.0


def test_profile_freshness_detects_newer_sources(tmp_path: Path):
    repo = tmp_path / "repo"
    src = repo / "src" / "module.cpp"
    src.parent.mkdir(parents=True)
    src.write_text("// v1\n", encoding="utf-8")
    art = tmp_path / "art"
    art.mkdir()
    (art / "profile_summary.json").write_text("{}", encoding="utf-8")
    time.sleep(0.05)
    src.write_text("// v2\n", encoding="utf-8")
    freshness = profile_freshness(
        art, repo_root=repo, editable_rel_paths=["src/module.cpp"]
    )
    assert freshness["stale"] is True
    assert any("editable sources newer" in r for r in freshness["reasons"])


def test_aux_digest_includes_scoreboard_and_phase_breakdown(tmp_path: Path):
    (tmp_path / "best_valid_baseline.json").write_text(
        json.dumps({"attempt_id": "a", "score": 3.5, "metrics": {"read_only_query_speedup": 3.5}}),
        encoding="utf-8",
    )
    summary = write_aux_profile_artifacts(
        tmp_path,
        {
            "read_only_query_speedup": 3.6,
            "accel_hit_median_us": 1000.0,
            "accel_hit_rate": 1.0,
        },
        source="test",
        bench_output=(
            'ACCEL_CUSTOM_PROFILE {"phase":"sort","us":600,"emitted_rows":100}\n'
            'ACCEL_CUSTOM_PROFILE {"phase":"count","us":200,"rows":500}\n'
        ),
    )
    assert summary["scoreboard"]["best_valid_score"] == 3.5
    assert summary["phase_breakdown"][0]["phase"] == "sort"
    digest = build_aux_profile_digest(tmp_path)
    assert digest["scoreboard"]["compare_against"] == "best_valid_score"
    assert digest["phase_breakdown"]
    assert "custom_profiling" in digest
    assert digest["work_counters_mean"]
    assert primary_score_from_metrics(
        {"read_only_query_speedup": 4.4}, is_aux=True
    ) == 4.4
