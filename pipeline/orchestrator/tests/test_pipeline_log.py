from __future__ import annotations

import json

import pytest

from accel_pipeline.pipeline_log import (
    events_path,
    format_candidate_summary,
    format_usage,
    log_analysis_candidates,
    log_base_bench_gate,
    log_bench_sweep_results,
    log_event,
    log_pipeline_summary,
    log_stage_complete,
    log_structural_reanalysis_request,
    usage_from_metadata,
)


def test_format_usage():
    assert format_usage(None) == "tokens=n/a"
    assert "total=1200" in format_usage(
        {"input_tokens": 1000, "output_tokens": 200, "total_tokens": 1200}
    )


def test_usage_from_metadata():
    assert usage_from_metadata({"usage": {"input_tokens": 1, "output_tokens": 2, "total_tokens": 3}})


def test_format_candidate_summary():
    selected = {
        "id": "c1",
        "viability": "viable",
        "selected": True,
        "design_family": "compact_join_intermediate",
        "state_grain": "segment × order",
        "read_path_bottleneck_risk": "medium",
        "asymptotic_read_complexity": "O(orders_in_segment)",
    }
    line = format_candidate_summary(selected)
    assert "c1" in line
    assert "[SELECTED]" in line
    assert "risk=medium" in line
    assert "residual=O(orders_in_segment)" in line

    rejected = {
        "id": "c5",
        "viability": "rejected",
        "design_family": "materialized_join_rows",
        "rejection_reason": "Full materialization too large for maintenance",
    }
    assert "rejected" in format_candidate_summary(rejected)
    assert "too large" in format_candidate_summary(rejected)


def test_log_analysis_candidates(capsys, tmp_path):
    analysis = {
        "fitness_assessment": {"recommended": True},
        "state_candidate_search": {
            "candidates": [
                {
                    "id": "c1",
                    "viability": "viable",
                    "selected": True,
                    "design_family": "compact_join_intermediate",
                    "state_grain": "segment × order",
                    "read_path_bottleneck_risk": "medium",
                },
                {
                    "id": "c2",
                    "viability": "viable",
                    "selected": False,
                    "design_family": "multilevel_hierarchical_summaries",
                    "best_regime": "Early cutoff prunes months",
                },
            ]
        },
    }
    log_analysis_candidates(tmp_path, 3, analysis)
    captured = capsys.readouterr()
    assert "analysis candidates" in captured.err
    assert "selected=c1" in captured.err
    assert "* c1 [SELECTED]" in captured.err
    assert "- c2" in captured.err

    events = events_path(tmp_path, 3).read_text(encoding="utf-8").strip().splitlines()
    payload = json.loads(events[-1])
    assert payload["event"] == "analysis_candidates"
    assert payload["selected_id"] == "c1"
    assert len(payload["lines"]) == 2


def test_log_bench_sweep_results(capsys, tmp_path):
    log_bench_sweep_results(
        tmp_path,
        12,
        gate_status="pass",
        gate_data={
            "composite_score": 20.483,
            "composite_workload_speedup": 1.21,
            "read_only_query_speedup": 192.73,
            "by_write_frequency_pct": {
                "0": {
                    "write_frequency_pct": 0,
                    "workload_speedup": 185.47,
                    "query_only_speedup": 192.73,
                    "accel_hit_rate": 1.0,
                    "writes_executed": 0,
                },
                "50": {
                    "write_frequency_pct": 50,
                    "workload_speedup": 1.21,
                    "query_only_speedup": 61.85,
                    "accel_hit_rate": 1.0,
                    "writes_executed": 50,
                },
            },
        },
    )
    captured = capsys.readouterr()
    assert "aux_bench_sweep status=pass" in captured.err
    assert "composite=20.483" in captured.err
    assert "0% writes" in captured.err
    assert "50% writes" in captured.err
    assert "185.47x vs DuckDB" in captured.err

    events = events_path(tmp_path, 12).read_text(encoding="utf-8").strip().splitlines()
    payload = json.loads(events[-1])
    assert payload["event"] == "aux_bench_sweep"
    assert len(payload["lines"]) == 2


def test_log_base_bench_gate_baseline(capsys, tmp_path):
    log_base_bench_gate(
        tmp_path,
        1,
        gate_name="baseline",
        gate_status="pass",
        gate_data={
            "log_tail": (
                "label,reference_ms,accelerator_ms,speedup,speedup_median\n"
                "summary_avg_median,432.0,728.0,0.59,0.59"
            )
        },
        change_kind="implementation",
        iteration=0,
        workload={"artifact_namespace": "sec_edgar"},
    )
    captured = capsys.readouterr()
    assert "gate baseline (implementation): status=pass speedup_median=0.590x" in captured.err
    assert "SLOWER than DuckDB" in captured.err


def test_log_base_bench_gate_profile(capsys, tmp_path):
    log_base_bench_gate(
        tmp_path,
        1,
        gate_name="profile",
        gate_status="pass",
        gate_data={
            "log_tail": (
                "label,pre_scan_us,pre_next_us,pre_consume_us,finalize_us,generic_rows,pre_rows\n"
                "q1_profile,100,200,3000,50,400000,1000000"
            )
        },
        change_kind="optimization",
        iteration=2,
        workload={"artifact_namespace": "sec_edgar"},
    )
    captured = capsys.readouterr()
    assert "gate profile (optimization iter=2)" in captured.err
    assert "pre_consume_us=3000us" in captured.err
    assert "generic_rows=400000" in captured.err


def test_log_event_writes_jsonl(tmp_path, capsys):
    log_event(tmp_path, 7, "test_event", foo="bar")
    captured = capsys.readouterr()
    assert "[pipeline] test_event: foo=bar" in captured.err
    lines = events_path(tmp_path, 7).read_text(encoding="utf-8").strip().splitlines()
    assert json.loads(lines[0])["event"] == "test_event"


def test_log_stage_complete_stderr(capsys, tmp_path):
    log_stage_complete(
        tmp_path,
        3,
        stage="analysis",
        model_id="composer-2.5",
        backend="cursor",
        duration_ms=45000,
        usage={"input_tokens": 100, "output_tokens": 50, "total_tokens": 150},
    )
    captured = capsys.readouterr()
    assert "stage complete: analysis" in captured.err
    assert "total=150" in captured.err
    assert "duration=45000ms" in captured.err


def test_structural_request_logging(capsys, tmp_path):
    log_structural_reanalysis_request(
        tmp_path,
        3,
        status="queued",
        source_stage="optimization",
        source_iteration=2,
        bottleneck_type="scan_for_scan",
        current_candidate_id="c1",
    )
    captured = capsys.readouterr()
    assert "structural reanalysis queued" in captured.err
    assert "scan_for_scan" in captured.err


def test_pipeline_summary_aggregates(tmp_path, capsys):
    log_stage_complete(
        tmp_path,
        1,
        stage="analysis",
        model_id="m",
        backend="cursor",
        duration_ms=1,
        usage={"input_tokens": 100, "output_tokens": 20, "total_tokens": 120},
    )
    log_stage_complete(
        tmp_path,
        1,
        stage="implementation",
        model_id="m",
        backend="cursor",
        duration_ms=1,
        usage={"input_tokens": 200, "output_tokens": 30, "total_tokens": 230},
    )
    log_structural_reanalysis_request(
        tmp_path,
        1,
        status="queued",
        source_stage="optimization",
        source_iteration=1,
        bottleneck_type="maintenance_infeasible",
        current_candidate_id="c1",
    )
    capsys.readouterr()
    log_pipeline_summary(tmp_path, 1)
    captured = capsys.readouterr()
    assert "summary: stages=2" in captured.err
    assert "total=350" in captured.err
    assert "structural_requests=1" in captured.err
