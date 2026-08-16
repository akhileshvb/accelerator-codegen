"""Tests for aux profile digest synthesis."""

from __future__ import annotations

import json
from pathlib import Path

from accel_pipeline.aux_profile import (
    build_aux_profile_digest,
    parse_custom_profile_events,
    write_aux_profile_artifacts,
)


def test_parse_custom_profile_events():
    output = """
noise
ACCEL_CUSTOM_PROFILE {"phase":"decode","decode_us":12.5}
ACCEL_CUSTOM_PROFILE: {"join_us":3.0}
"""
    events = parse_custom_profile_events(output)
    assert len(events) == 2
    assert events[0]["decode_us"] == 12.5
    assert events[1]["join_us"] == 3.0


def test_write_and_digest_aux_profile(tmp_path: Path):
    metrics = {
        "read_only_query_speedup": 3.47,
        "composite_score": 3.56,
        "accel_hit_median_us": 11491.0,
        "accel_hit_rate": 1.0,
        "valid_partitions": 51,
        "partitions_read_per_query_median": 9.0,
        "store_diagnostics": {
            "accel_hit_median_us": 11491.0,
            "accel_hit_rate": 1.0,
            "valid_partitions": 51,
            "partitions_read_per_query_median": 9.0,
        },
    }
    summary = write_aux_profile_artifacts(
        tmp_path,
        metrics,
        source="test",
        bench_output='ACCEL_CUSTOM_PROFILE {"decode_us":100.0}\n',
    )
    assert summary["workload_kind"] == "aux_state"
    assert summary["read_only_query_speedup"] == 3.47
    assert summary["dominant_phase"] in {"accel_hit_us", "decode", "decode_us"}
    assert (tmp_path / "profile_summary.json").is_file()
    assert (tmp_path / "baseline_summary.json").is_file()

    (tmp_path / "aux_bench_summary.json").write_text(
        json.dumps({"read_only_query_speedup": 3.47, "composite_score": 3.56}),
        encoding="utf-8",
    )
    digest = build_aux_profile_digest(tmp_path)
    assert digest["profile_summary"]["read_only_query_speedup"] == 3.47
    assert digest["fair_sweep"]["read_only_query_speedup"] == 3.47
    assert digest["store_diagnostics"]["accel_hit_median_us"] == 11491.0
    assert "custom_profiling" in digest
    assert digest["scoreboard"]["compare_against"] == "best_valid_score"
