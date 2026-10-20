"""The ThreadSanitizer gates are registered, allow-listed and planned."""

from __future__ import annotations

import re
from pathlib import Path

from accel_pipeline.config import load_workload
from accel_pipeline.gates import GATES_ALLOW_SKIP, GATE_SCRIPTS
from accel_pipeline.stages.gate_plan import workload_post_audit_gates

REPO_ROOT = Path(__file__).resolve().parents[3]


def _allowed(workload: dict, command: str) -> bool:
    return any(re.search(pattern, command) for pattern in workload["allowed_commands"])


def test_tsan_gates_map_to_the_script():
    assert GATE_SCRIPTS["aux_tsan"] == GATE_SCRIPTS["slot_tsan"] == "run_aux_tsan.sh"
    assert (REPO_ROOT / "pipeline/gates" / GATE_SCRIPTS["aux_tsan"]).is_file()
    assert {"aux_tsan", "slot_tsan"} <= GATES_ALLOW_SKIP


def test_aux_state_audit_runs_tsan():
    for name in ("tpch_q3_aux", "sec_edgar_q1_aux"):
        workload = load_workload(name, REPO_ROOT, engine="duckdb")
        assert "aux_tsan" in workload_post_audit_gates(workload)
        assert _allowed(workload, "bash pipeline/gates/run_aux_tsan.sh 3")


def test_engine_backend_validation_runs_slot_tsan():
    workload = load_workload("engine_mysql", REPO_ROOT)
    assert "slot_tsan" in workload["post_validation_gates"]
    assert _allowed(workload, "bash pipeline/gates/run_aux_tsan.sh slots")
