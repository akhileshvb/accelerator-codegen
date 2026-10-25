from __future__ import annotations

import json
from pathlib import Path

from accel_pipeline.gate_capture import (
    describe_returncode,
    enrich_failed_gate_data,
    log_tail_for_agent,
)
from accel_pipeline.gates import GateResult
from accel_pipeline.engine_backend.stage import format_layer_gate_feedback
from accel_pipeline.tools.gate_tools import _compact_gate_payload


def test_describe_abort_returncode():
    assert "SIGABRT" in describe_returncode(134)
    assert "SIGABRT" in describe_returncode(-6)


def test_log_tail_for_agent_never_empty_on_abort():
    text = log_tail_for_agent(
        {
            "notes": "layer harness failed via docker exec: test_mysql_epochs",
            "log_tail": "",
            "returncode": 134,
        }
    )
    assert "no captured stdout/stderr" in text
    assert "134" in text
    assert "SIGABRT" in text
    assert "test_mysql_epochs" in text


def test_enrich_failed_gate_prefers_json_returncode_and_fills_tail():
    out = enrich_failed_gate_data(
        {"status": "fail", "notes": "harness failed", "log_tail": "", "returncode": 134},
        proc_returncode=1,
        proc_output="",
    )
    assert out["returncode"] == 134
    assert "SIGABRT" in out["log_tail"]


def test_enrich_failed_gate_uses_wrapper_output_when_json_tail_empty():
    out = enrich_failed_gate_data(
        {"status": "fail", "notes": "harness failed", "log_tail": ""},
        proc_returncode=1,
        proc_output="FUNCTION accel_mysql_test.mysql_accel_txn_init does not exist\n",
    )
    assert "mysql_accel_txn_init" in out["log_tail"]
    assert out["returncode"] == 1


def test_layer_gate_feedback_includes_abort_when_log_tail_empty(tmp_path):
    failed = GateResult(
        name="engine_epochs",
        status="fail",
        data={
            "notes": "layer harness failed via docker exec: test_mysql_epochs",
            "log_tail": "",
            "returncode": 134,
        },
        path=tmp_path / "engine_epochs.json",
    )
    text = format_layer_gate_feedback(
        failed, layer_id="txn", round_idx=2, max_rounds=3
    )
    assert "Layer gate FAILED" in text
    assert "### returncode" in text
    assert "SIGABRT" in text
    assert "### log_tail" in text
    assert "(empty)" not in text
    assert "attempt: 2/3" in text


def test_compact_gate_payload_always_has_failure_summary():
    compact = _compact_gate_payload(
        "engine_epochs",
        {
            "status": "fail",
            "notes": "layer harness failed via docker exec: test_mysql_epochs",
            "log_tail": "",
            "returncode": 134,
        },
    )
    assert compact["returncode"] == 134
    assert "SIGABRT" in compact["failure_summary"]
    assert compact["failure_summary"].strip()


def test_run_gate_persists_enriched_fail_json(monkeypatch, tmp_path):
    from accel_pipeline.config import load_pipeline_config, load_workload
    from accel_pipeline.gates import run_gate

    repo = Path(__file__).resolve().parents[3]
    pc = load_pipeline_config(repo_root=repo)
    workload = load_workload("engine_mysql", repo)
    captured: dict = {}

    def fake_run(cmd, **kwargs):
        captured["env"] = kwargs.get("env") or {}
        out = Path(captured["env"]["ACCEL_GATE_OUT"])
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(
            json.dumps(
                {
                    "gate": "engine_epochs",
                    "status": "fail",
                    "notes": "layer harness failed via docker exec: test_mysql_epochs",
                    "log_tail": "",
                    "returncode": 134,
                }
            ),
            encoding="utf-8",
        )

        class Proc:
            returncode = 1
            stdout = ""
            stderr = ""

        return Proc()

    monkeypatch.setattr(
        "accel_pipeline.gates._run_gate_script",
        lambda script, query, cwd, env: fake_run(["bash", str(script), str(query)], cwd=cwd, env=env),
    )
    monkeypatch.setattr(
        "accel_pipeline.pack.worker_isolation.ensure_worker_build_dir",
        lambda *a, **k: None,
    )
    result = run_gate(pc, "engine_epochs", 0, workload=workload, raise_on_failure=False)
    assert result.status == "fail"
    assert "SIGABRT" in result.data["log_tail"]
    persisted = json.loads(result.path.read_text(encoding="utf-8"))
    assert "SIGABRT" in persisted["log_tail"]
    assert persisted["returncode"] == 134
