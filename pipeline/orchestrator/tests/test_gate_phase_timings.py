"""Phase timings of a gate: the script's sidecar ends up in the gate JSON, in GateResult.data and in the log."""

from __future__ import annotations

import json
from pathlib import Path

from accel_pipeline.config import load_pipeline_config, load_workload
from accel_pipeline.gates import read_phase_log, run_gate


def test_read_phase_log_adds_up_repeated_phases(tmp_path):
    gate_json = tmp_path / "aux_unit.json"
    gate_json.with_suffix(".phases").write_text("load_context=0.1\nrun_ctest=2.5\nrun_ctest=1.5\n")
    assert read_phase_log(gate_json) == {"load_context": 0.1, "run_ctest": 4.0}


def test_a_missing_sidecar_is_no_phases(tmp_path):
    assert read_phase_log(tmp_path / "none.json") == {}


def run_with_fake_script(monkeypatch, status, rc):
    repo = Path(__file__).resolve().parents[3]
    pc = load_pipeline_config(repo_root=repo)
    profile = sorted((repo / "pipeline" / "config" / "workloads").glob("engine_[a-z]*.yaml"))[0]
    workload = load_workload(profile.stem, repo)

    def fake_script(script, query, cwd, env):
        out = Path(env["ACCEL_GATE_OUT"])
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps({"gate": "engine_epochs", "status": status, "log_tail": "x", "returncode": rc}))
        out.with_suffix(".phases").write_text("load_context=0.25\nhost_build=3.0\nrun_x=1.0\n")

        class Proc:
            returncode = rc
            stdout = ""
            stderr = ""

        return Proc()

    monkeypatch.setattr("accel_pipeline.gates._run_gate_script", fake_script)
    monkeypatch.setattr("accel_pipeline.pack.worker_isolation.ensure_worker_build_dir", lambda *a, **k: None)
    return run_gate(pc, "engine_epochs", 0, workload=workload, raise_on_failure=False)


def test_run_gate_puts_phase_timings_in_the_result_and_the_gate_json(monkeypatch):
    result = run_with_fake_script(monkeypatch, "pass", 0)
    assert result.data["phase_timings"]["host_build"] == 3.0
    assert "gate_wall" in result.data["phase_timings"]
    assert json.loads(result.path.read_text())["phase_timings"]["run_x"] == 1.0


def test_a_failing_gate_keeps_its_phase_timings(monkeypatch):
    result = run_with_fake_script(monkeypatch, "fail", 1)
    assert result.status == "fail"
    assert json.loads(result.path.read_text())["phase_timings"]["load_context"] == 0.25


def test_pipeline_log_prints_the_phases(capsys):
    from accel_pipeline.pipeline_log import log_gate_complete

    log_gate_complete("aux_unit", status="pass", duration_ms=5000, phase_timings={"host_build": 3.0, "gate_wall": 5.0})
    err = capsys.readouterr().err
    assert "phases: host_build=3.0s" in err and "gate_wall" not in err.split("phases:")[1]
