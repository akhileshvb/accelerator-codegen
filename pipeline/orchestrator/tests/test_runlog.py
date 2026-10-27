from __future__ import annotations

import json


from accel_pipeline.runlog import (
    events_path,
    log_event,
    log_gate_batch_start,
    log_gate_complete,
    log_gate_start,
    log_pipeline_summary,
    log_stage_complete,
    log_structural_reanalysis_request,
    session_usage,
    usage_table,
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


# ---- token usage per codex session (the basis of the timeline's cost) ----


def rollout(home, session_id, lines):
    path = home / "sessions/2026/10/25" / f"rollout-2026-10-25T00-00-00-{session_id}.jsonl"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(json.dumps(line) + "\n" for line in lines))


def tokens(total_in, total_out, last_in, last_out):
    usage = lambda i, o: {"input_tokens": i, "cached_input_tokens": 0, "output_tokens": o, "reasoning_output_tokens": 0}
    return {"timestamp": "2026-10-25T00:00:01Z", "type": "event_msg",
            "payload": {"type": "token_count", "info": {"total_token_usage": usage(total_in, total_out),
                                                        "last_token_usage": usage(last_in, last_out)}}}


def test_a_fork_counts_only_its_own_responses_not_the_parents_running_total(tmp_path):
    meta = {"timestamp": "2026-10-25T00:00:00Z", "type": "session_meta", "payload": {"id": "fork", "forked_from_id": "main"}}
    rollout(tmp_path, "fork", [meta, tokens(8_000_100, 1_010, 100, 10), tokens(8_000_300, 1_030, 200, 20)])
    usage = session_usage(tmp_path, "fork", 0.0)
    assert (usage["input_tokens"], usage["output_tokens"], usage["model_calls"]) == (300, 30, 2)


def test_a_session_of_its_own_counts_from_zero(tmp_path):
    meta = {"timestamp": "2026-10-25T00:00:00Z", "type": "session_meta", "payload": {"id": "main"}}
    rollout(tmp_path, "main", [meta, tokens(100, 10, 100, 10), tokens(300, 30, 200, 20)])
    assert session_usage(tmp_path, "main", 0.0)["input_tokens"] == 300


def test_the_usage_table_sums_stage_completes_per_stage(tmp_path):
    log_stage_complete(tmp_path, 3, WORKLOAD, stage="verus_kernel", model_id="m", backend="codex_exec",
                       duration_ms=120_000, usage={"input_tokens": 2_000_000, "output_tokens": 5_000,
                                                   "cache_read_tokens": 1_000_000, "model_calls": 4,
                                                   "tool_calls": 9, "cost_usd": 1.5})
    log_stage_complete(tmp_path, 3, WORKLOAD, stage="verus_kernel", model_id="m", backend="codex_exec",
                       duration_ms=60_000, usage={"input_tokens": 1_000_000, "cost_usd": 0.5})
    rows = usage_table(tmp_path, 3, WORKLOAD).splitlines()
    kernel = next(r for r in rows if r.startswith("verus_kernel"))
    assert kernel.split()[2:4] == ["2", "3.0"] and kernel.split()[-1] == "2.00"
    assert rows[-1].startswith("TOTAL")
    assert usage_table(tmp_path, 4, WORKLOAD) == ""
