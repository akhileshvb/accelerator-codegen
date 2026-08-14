"""Tests for graded attempt outcomes."""

from __future__ import annotations

from accel_pipeline.optimization_eval import EvalConfig, evaluate_attempt_outcome


def test_accept_on_read_improvement():
    cfg = EvalConfig(improvement_threshold_pct=3.0, reject_threshold_pct=2.0)
    outcome = evaluate_attempt_outcome(
        baseline_score=10.0,
        candidate_score=11.0,
        baseline_metrics={"read_only_query_speedup": 10.0, "valid_partitions": 1000},
        candidate_metrics={"read_only_query_speedup": 11.0, "valid_partitions": 1000, "accel_hit_rate": 1.0},
        config=cfg,
        correctness_ok=True,
    )
    assert outcome.verdict == "accept"


def test_keep_explore_on_structural_progress():
    cfg = EvalConfig(improvement_threshold_pct=3.0, reject_threshold_pct=2.0)
    outcome = evaluate_attempt_outcome(
        baseline_score=10.0,
        candidate_score=10.1,
        baseline_metrics={
            "read_only_query_speedup": 10.0,
            "valid_partitions": 10000,
            "validity_slots_scanned_median": 10000,
            "accel_hit_rate": 1.0,
        },
        candidate_metrics={
            "read_only_query_speedup": 10.1,
            "valid_partitions": 1000,
            "validity_slots_scanned_median": 1000,
            "accel_hit_rate": 1.0,
        },
        config=cfg,
        correctness_ok=True,
        structural_progress_threshold_pct=5.0,
    )
    assert outcome.verdict == "keep_explore"


def test_reject_broken_on_failed_correctness():
    cfg = EvalConfig()
    outcome = evaluate_attempt_outcome(
        baseline_score=10.0,
        candidate_score=0.0,
        baseline_metrics={},
        candidate_metrics={},
        config=cfg,
        correctness_ok=False,
    )
    assert outcome.verdict == "reject_broken"
