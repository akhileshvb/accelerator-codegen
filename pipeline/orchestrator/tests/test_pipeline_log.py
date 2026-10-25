from __future__ import annotations

import json


from accel_pipeline.pipeline_log import (
    events_path,
    log_event,
    log_gate_batch_start,
    log_gate_complete,
    log_gate_start,
    log_pipeline_summary,
    log_stage_complete,
    log_structural_reanalysis_request,
)

WORKLOAD = {"benchmark_name": "tpch", "engine_name": "duckdb"}


def test_log_gate_batch_and_complete(capsys, tmp_path):
    log_gate_batch_start(
        17,
        ["aux_correctness", "aux_bench_sweep"],
        change_kind="implementation",
    )
    log_gate_start("aux_correctness", 17, change_kind="implementation", index=1, count=2)
    log_gate_complete(
        "aux_correctness",
        status="pass",
        duration_ms=120,
        change_kind="implementation",
        index=1,
        count=2,
    )
    captured = capsys.readouterr()
    assert "gates (implementation, Q17): 2 to run" in captured.err
    assert "1. aux_correctness" in captured.err
    assert "aux_bench_sweep" in captured.err
    assert "gate start: [1/2] aux_correctness" in captured.err
    assert "gate complete: [1/2] aux_correctness" in captured.err
    assert "status=pass duration=120ms" in captured.err


def test_log_event_writes_jsonl(tmp_path, capsys):
    log_event(tmp_path, 7, WORKLOAD, "test_event", foo="bar")
    captured = capsys.readouterr()
    assert "[pipeline] test_event: foo=bar" in captured.err
    lines = events_path(tmp_path, 7, WORKLOAD).read_text(encoding="utf-8").strip().splitlines()
    assert json.loads(lines[0])["event"] == "test_event"


def test_pipeline_summary_aggregates(tmp_path, capsys):
    log_stage_complete(
        tmp_path,
        1,
        WORKLOAD,
        stage="analysis",
        model_id="m",
        backend="codex_exec",
        duration_ms=1,
        usage={"input_tokens": 100, "output_tokens": 20, "total_tokens": 120},
    )
    log_stage_complete(
        tmp_path,
        1,
        WORKLOAD,
        stage="implementation",
        model_id="m",
        backend="codex_exec",
        duration_ms=1,
        usage={"input_tokens": 200, "output_tokens": 30, "total_tokens": 230},
    )
    log_structural_reanalysis_request(
        tmp_path,
        1,
        WORKLOAD,
        status="queued",
        source_stage="optimization",
        source_iteration=1,
        bottleneck_type="maintenance_infeasible",
        current_candidate_id="c1",
    )
    capsys.readouterr()
    log_pipeline_summary(tmp_path, 1, WORKLOAD)
    captured = capsys.readouterr()
    assert "summary: stages=2" in captured.err
    assert "total=350" in captured.err
    assert "structural_requests=1" in captured.err
