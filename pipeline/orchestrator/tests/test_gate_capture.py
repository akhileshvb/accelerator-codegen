from __future__ import annotations


from accel_pipeline.gates.run import (
    enrich_failed_gate_data,
    log_tail_for_agent,
)
from accel_pipeline.tools.gate_tools import _compact_gate_payload


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


def test_enrich_failed_gate_uses_wrapper_output_when_json_tail_empty():
    out = enrich_failed_gate_data(
        {"status": "fail", "notes": "harness failed", "log_tail": ""},
        proc_returncode=1,
        proc_output="FUNCTION accel_mysql_test.mysql_accel_txn_init does not exist\n",
    )
    assert "mysql_accel_txn_init" in out["log_tail"]
    assert out["returncode"] == 1


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
