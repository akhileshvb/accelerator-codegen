"""The quick_check, background-gate and budget-footer parts of the MCP tool surface."""

from __future__ import annotations

import json
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace

from accel_pipeline import mcp_server
from accel_pipeline.config import QuickTierConfig, load_pipeline_config
from accel_pipeline.gates import QuickResult
from accel_pipeline.repair import Failure
from accel_pipeline.tools import gate_tools
from accel_pipeline.tools.gate_tools import ToolContext

CONFIG = SimpleNamespace(quick_tier=QuickTierConfig())
WORKLOAD = {"workload_kind": "aux_state"}


def test_quick_check_passes_its_arguments_on_and_renders_the_result(monkeypatch):
    seen = {}

    def fake(config, workload, query, *, targets, differential, sanitizer, threads):
        seen.update(query=query, targets=targets, differential=differential, sanitizer=sanitizer, threads=threads)
        return QuickResult("pass", {"host_build": 0.5}, summary="quick_check passed")

    monkeypatch.setattr(gate_tools, "run_quick_check", fake)
    quick_check = gate_tools.make_quick_tools(ToolContext(CONFIG, 6, WORKLOAD))
    out = json.loads(quick_check(["my_test"], False, "thread", 6))
    assert seen == {"query": 6, "targets": ["my_test"], "differential": False, "sanitizer": "thread", "threads": 6}
    assert out == {"status": "pass", "summary": "quick_check passed", "phase_seconds": {"host_build": 0.5}}


def test_a_failed_quick_check_shows_the_failure_and_how_to_reproduce(monkeypatch):
    failure = Failure("code", "quick_check", "quick_check", "differential failed", evidence="row 3", reproduce="host.sh run d 6")
    monkeypatch.setattr(gate_tools, "run_quick_check", lambda *a, **k: QuickResult("fail", failure=failure, summary="x"))
    out = json.loads(gate_tools.make_quick_tools(ToolContext(CONFIG, 6, WORKLOAD))())
    assert out["status"] == "fail" and "Reproduce: `host.sh run d 6`" in out["failure"]


def test_request_gate_in_the_background_starts_it_and_returns_at_once(monkeypatch):
    started = {}

    def fake_start(config, workload, query, gate, *, workload_name):
        started.update(gate=gate, workload_name=workload_name)
        return {"gate": gate, "status": "running", "pid": 1, "stamp_start": "s"}

    monkeypatch.setattr(gate_tools, "start_background_gate", fake_start)
    monkeypatch.setattr(gate_tools, "run_gate", lambda *a, **k: (_ for _ in ()).throw(AssertionError("ran in the foreground")))
    request_gate, _ = gate_tools.make_gate_tools(ToolContext(CONFIG, 6, WORKLOAD, workload_name="tpch_q6_aux"))
    out = json.loads(request_gate("aux_unit", background=True))
    assert started == {"gate": "aux_unit", "workload_name": "tpch_q6_aux"}
    assert out == {"gate": "aux_unit", "status": "running"}


def test_gate_status_reports_a_finished_gate_compactly_and_whether_it_is_stale(monkeypatch):
    state = {"gate": "aux_unit", "status": "fail", "stale": True,
             "data": {"status": "fail", "reason": "boom", "log_tail": "tail"}}
    monkeypatch.setattr(gate_tools, "background_status", lambda *a: state)
    _, gate_status = gate_tools.make_gate_tools(ToolContext(CONFIG, 6, WORKLOAD))
    out = json.loads(gate_status("aux_unit"))
    assert out["status"] == "fail" and out["stale"] is True and out["reason"] == "boom"
    assert "log_tail" not in out and "data" not in out


def test_the_budget_footer_is_appended_after_a_blank_line(monkeypatch):
    monkeypatch.setattr("accel_pipeline.session.budget_footer", lambda config, stage, query: "WRAP UP: checkpoint now")
    assert mcp_server._budget_footer(CONFIG, "optimization", 6) == "\n\nWRAP UP: checkpoint now"


def test_an_empty_footer_adds_nothing_and_a_broken_one_never_fails_the_call(monkeypatch):
    monkeypatch.setattr("accel_pipeline.session.budget_footer", lambda config, stage, query: "")
    assert mcp_server._budget_footer(CONFIG, "s", 1) == ""

    def broken(config, stage, query):
        raise RuntimeError("no session")

    monkeypatch.setattr("accel_pipeline.session.budget_footer", broken)
    assert mcp_server._budget_footer(CONFIG, "s", 1) == ""


def test_heavy_gates_are_refused_before_a_passing_quick_check_in_the_foreground_and_the_background(monkeypatch, tmp_path):
    monkeypatch.setattr(gate_tools, "start_background_gate", lambda *a, **k: (_ for _ in ()).throw(AssertionError("started")))
    monkeypatch.setattr(gate_tools, "run_gate", lambda *a, **k: (_ for _ in ()).throw(AssertionError("ran")))
    config = SimpleNamespace(repo_root=tmp_path, quick_tier=CONFIG.quick_tier)  # not a git tree: no stamp, no admission
    request_gate, _ = gate_tools.make_gate_tools(ToolContext(config, 6, WORKLOAD))
    for background in (False, True):
        out = json.loads(request_gate("aux_correctness", background=background))
        assert out["status"] == "refused" and "run quick_check first" in out["reason"]


def test_an_admitted_gate_runs(monkeypatch):
    ran = []
    monkeypatch.setattr(gate_tools, "run_gate", lambda config, name, *a, **k: ran.append(name) or SimpleNamespace(name=name, data={"status": "pass"}))
    monkeypatch.setattr(gate_tools, "admission_refusal_payload", lambda ctx, gate: "")
    request_gate, *_ = gate_tools.make_gate_tools(ToolContext(CONFIG, 6, WORKLOAD))
    assert json.loads(request_gate("aux_correctness"))["status"] == "pass" and ran == ["aux_correctness"]


def test_run_correctness_is_refused_like_request_gate(tmp_path):
    root = Path(__file__).resolve().parents[3]
    config = replace(load_pipeline_config(repo_root=root), repo_root=tmp_path)
    tools = mcp_server.build_mcp_tool_callables(config, WORKLOAD, "w", 6, "implementation", 0, tmp_path)
    out = json.loads(tools["run_correctness"]())
    assert out["gate"] == "aux_correctness" and out["status"] == "refused"
