"""Tests for one-shot host_bootstrap stage."""

from __future__ import annotations

from pathlib import Path

import json

import pytest

from accel_pipeline.config import (
    load_model_config,
    load_pipeline_config,
    load_workload,
)
from accel_pipeline.gates import GateFailure, GateResult
from accel_pipeline.host_bootstrap import (
    engines_needing_bootstrap_from_workloads,
    format_host_bootstrap_gate_feedback,
    host_bootstrap_passed,
    needs_host_bootstrap,
    run_host_bootstrap,
    write_host_bootstrap_status,
)
from accel_pipeline.stages.base import resolve_stage_max_turns


REPO_ROOT = Path(__file__).resolve().parents[3]


def test_duckdb_does_not_need_host_bootstrap():
    w = load_workload("tpch_q3", REPO_ROOT, engine="duckdb")
    assert needs_host_bootstrap(w) is False


def test_postgres_needs_host_bootstrap():
    w = load_workload("tpch_q3", REPO_ROOT, engine="postgres")
    assert needs_host_bootstrap(w) is True


def test_host_bootstrap_status_roundtrip(tmp_path, monkeypatch):
    """Never write pass markers into the real repo artifacts tree."""
    engine = "postgres"

    def _fake_dir(repo_root: Path, engine_name: str) -> Path:
        path = tmp_path / "engines" / engine_name / "host_bootstrap"
        path.mkdir(parents=True, exist_ok=True)
        return path

    monkeypatch.setattr(
        "accel_pipeline.host_bootstrap.host_bootstrap_dir",
        _fake_dir,
    )
    # Differential source present so passed() accepts the marker.
    diff = tmp_path / "tests" / "aux_state" / "test_postgres_tpch_differential.cpp"
    diff.parent.mkdir(parents=True, exist_ok=True)
    diff.write_text("// stub\n", encoding="utf-8")
    monkeypatch.setattr(
        "accel_pipeline.host_bootstrap.engine_host_conventions",
        lambda _e, repo_root=None: {
            "agent_builds_query_host": True,
            "differential_source": str(diff.relative_to(tmp_path)),
        },
    )
    write_host_bootstrap_status(tmp_path, engine, status="pass", reason="gate")
    assert host_bootstrap_passed(tmp_path, engine) is True


def test_stale_unit_marker_does_not_count_as_passed(tmp_path, monkeypatch):
    def _fake_dir(repo_root: Path, engine_name: str) -> Path:
        path = tmp_path / "engines" / engine_name / "host_bootstrap"
        path.mkdir(parents=True, exist_ok=True)
        return path

    monkeypatch.setattr("accel_pipeline.host_bootstrap.host_bootstrap_dir", _fake_dir)
    monkeypatch.setattr(
        "accel_pipeline.host_bootstrap.engine_host_conventions",
        lambda _e, repo_root=None: {"agent_builds_query_host": True, "differential_source": ""},
    )
    write_host_bootstrap_status(tmp_path, "postgres", status="pass", reason="unit")
    assert host_bootstrap_passed(tmp_path, "postgres") is False


def test_engines_needing_bootstrap_per_run_engine():
    runs = [("tpch_q3_aux", "duckdb"), ("tpch_q3_aux", "postgres"), ("tpch_q1_aux", "postgres")]
    needed = engines_needing_bootstrap_from_workloads(REPO_ROOT, runs)
    engines = [e for e, _ in needed]
    assert "duckdb" not in engines
    assert engines.count("postgres") <= 1
    assert "postgres" in engines or host_bootstrap_passed(REPO_ROOT, "postgres")


def test_aux_workload_merges_host_bootstrap_retry_config():
    w = load_workload("tpch_q1_aux", REPO_ROOT, engine="postgres")
    assert w["host_bootstrap_gate_max_rounds"] == 3
    assert w["host_bootstrap_critique_max_rounds"] == 2
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    from accel_pipeline.config import apply_workload_overrides

    merged = apply_workload_overrides(pc, w)
    assert resolve_stage_max_turns(merged.stage_max_turns, "host_bootstrap") == 10_000


def test_host_bootstrap_gate_script_goes_through_the_engine_host():
    text = (REPO_ROOT / "pipeline" / "gates" / "run_host_bootstrap.sh").read_text(
        encoding="utf-8"
    )
    assert "engine_ready" in text
    assert "docker" not in text.lower()


def test_host_bootstrap_gate_feedback_includes_log_tail(tmp_path):
    failed = GateResult(
        name="host_bootstrap",
        status="fail",
        data={
            "reason": "docker build failed for test_postgres_tpch_differential",
            "log_tail": "ninja: error: unknown target 'test_postgres_tpch_differential'",
        },
        path=tmp_path / "host_bootstrap.json",
    )
    text = format_host_bootstrap_gate_feedback(failed, round_idx=1, max_rounds=3)
    assert "Host bootstrap gate FAILED" in text
    assert "unknown target" in text
    assert "attempt: 1/3" in text
    assert "request_gate" in text
    assert "### returncode" in text


def test_host_bootstrap_gate_feedback_empty_log_tail_still_has_signal():
    failed = GateResult(
        name="host_bootstrap",
        status="fail",
        data={
            "reason": "docker exec aborted",
            "log_tail": "",
            "returncode": 134,
        },
    )
    text = format_host_bootstrap_gate_feedback(failed, round_idx=2, max_rounds=3)
    assert "SIGABRT" in text
    assert "docker exec aborted" in text
    assert "(empty)" not in text


def _patch_bootstrap_dirs(tmp_path, monkeypatch):
    def _fake_dir(repo_root, engine_name):
        path = tmp_path / "engines" / engine_name / "host_bootstrap"
        path.mkdir(parents=True, exist_ok=True)
        return path

    monkeypatch.setattr("accel_pipeline.host_bootstrap.host_bootstrap_dir", _fake_dir)
    monkeypatch.setattr(
        "accel_pipeline.playbook.append_experiment_log", lambda *a, **k: tmp_path
    )
    return _fake_dir


def test_host_bootstrap_gate_fail_reinvokes(tmp_path, monkeypatch):
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    mc = load_model_config(repo_root=REPO_ROOT)
    _patch_bootstrap_dirs(tmp_path, monkeypatch)
    calls = {"stage": 0, "gates": 0, "critique": 0}
    user_messages: list[str] = []

    real_load = load_workload

    def fake_load(name, repo_root=None, *, engine=""):
        w = dict(real_load(name, repo_root, engine=engine))
        w["host_bootstrap_gate_max_rounds"] = 3
        w["host_bootstrap_critique_max_rounds"] = 2
        return w

    def fake_stage(*args, **kwargs):
        calls["stage"] += 1
        user_messages.append(str(kwargs.get("extra_user_message") or kwargs.get("user_message") or ""))
        return object()

    def fake_gate(*args, **kwargs):
        calls["gates"] += 1
        if calls["gates"] == 1:
            return GateResult(
                name="host_bootstrap",
                status="fail",
                data={
                    "reason": "docker build failed for test_postgres_tpch_differential",
                    "log_tail": "ninja: error: unknown target 'test_postgres_tpch_differential'",
                },
                path=tmp_path / "host_bootstrap.json",
            )
        return GateResult(
            name="host_bootstrap",
            status="pass",
            data={"reason": "built in docker"},
            path=tmp_path / "host_bootstrap.json",
        )

    def fake_critique(*args, **kwargs):
        calls["critique"] += 1
        art = tmp_path / "engines" / "postgres" / "host_bootstrap"
        art.mkdir(parents=True, exist_ok=True)
        (art / "host_bootstrap_critique.json").write_text(
            json.dumps(
                {
                    "engine_name": "postgres",
                    "status": "pass",
                    "summary": "ok",
                    "violations": [],
                    "feedback_for_implementation": "",
                }
            )
            + "\n",
            encoding="utf-8",
        )

    monkeypatch.setattr("accel_pipeline.host_bootstrap.load_workload", fake_load)
    monkeypatch.setattr("accel_pipeline.stages.host_bootstrap.run", fake_stage)
    monkeypatch.setattr("accel_pipeline.gates.run_gate", fake_gate)
    monkeypatch.setattr(
        "accel_pipeline.stages.host_bootstrap_critique.run_host_bootstrap_critique_agent",
        fake_critique,
    )

    out = run_host_bootstrap(
        pc,
        mc,
        engine_name="postgres",
        representative_workload="tpch_q1_aux",
        force=True,
    )
    assert out["status"] == "pass"
    assert calls["stage"] == 2
    assert calls["gates"] == 2
    assert calls["critique"] == 1
    assert "unknown target" in user_messages[1]
    assert (tmp_path / "engines" / "postgres" / "host_bootstrap" / "host_bootstrap_gate_feedback_round1.md").is_file()


def test_host_bootstrap_gate_exhausted_rounds_raises(tmp_path, monkeypatch):
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    mc = load_model_config(repo_root=REPO_ROOT)
    _patch_bootstrap_dirs(tmp_path, monkeypatch)
    calls = {"stage": 0}

    real_load = load_workload

    def fake_load(name, repo_root=None, *, engine=""):
        w = dict(real_load(name, repo_root, engine=engine))
        w["host_bootstrap_gate_max_rounds"] = 1
        w["host_bootstrap_critique_max_rounds"] = 0
        return w

    def fake_stage(*args, **kwargs):
        calls["stage"] += 1
        return object()

    def fake_gate(*args, **kwargs):
        return GateResult(
            name="host_bootstrap",
            status="fail",
            data={"reason": "still missing target", "log_tail": "make: stop"},
            path=tmp_path / "host_bootstrap.json",
        )

    monkeypatch.setattr("accel_pipeline.host_bootstrap.load_workload", fake_load)
    monkeypatch.setattr("accel_pipeline.stages.host_bootstrap.run", fake_stage)
    monkeypatch.setattr("accel_pipeline.gates.run_gate", fake_gate)

    with pytest.raises(GateFailure, match="still failing after 1"):
        run_host_bootstrap(
            pc,
            mc,
            engine_name="postgres",
            representative_workload="tpch_q1_aux",
            force=True,
        )
    assert calls["stage"] == 1


def test_host_bootstrap_critique_fail_reinvokes(tmp_path, monkeypatch):
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    mc = load_model_config(repo_root=REPO_ROOT)
    _patch_bootstrap_dirs(tmp_path, monkeypatch)
    calls = {"stage": 0, "critique": 0}

    real_load = load_workload

    def fake_load(name, repo_root=None, *, engine=""):
        w = dict(real_load(name, repo_root, engine=engine))
        w["host_bootstrap_gate_max_rounds"] = 3
        w["host_bootstrap_critique_max_rounds"] = 2
        return w

    def fake_stage(*args, **kwargs):
        calls["stage"] += 1
        return object()

    def fake_gate(*args, **kwargs):
        return GateResult(
            name="host_bootstrap",
            status="pass",
            data={"reason": "built in docker"},
            path=tmp_path / "host_bootstrap.json",
        )

    def fake_critique(*args, **kwargs):
        calls["critique"] += 1
        art = tmp_path / "engines" / "postgres" / "host_bootstrap"
        art.mkdir(parents=True, exist_ok=True)
        status = "fail" if calls["critique"] == 1 else "pass"
        (art / "host_bootstrap_critique.json").write_text(
            json.dumps(
                {
                    "engine_name": "postgres",
                    "status": status,
                    "summary": "stub harness" if status == "fail" else "ok",
                    "violations": (
                        [
                            {
                                "rule": "differential",
                                "evidence": "harness never LOADs MODULE",
                            }
                        ]
                        if status == "fail"
                        else []
                    ),
                    "feedback_for_implementation": (
                        "Call LOAD accelerator_codegen_pg in the harness"
                        if status == "fail"
                        else ""
                    ),
                }
            )
            + "\n",
            encoding="utf-8",
        )

    monkeypatch.setattr("accel_pipeline.host_bootstrap.load_workload", fake_load)
    monkeypatch.setattr("accel_pipeline.stages.host_bootstrap.run", fake_stage)
    monkeypatch.setattr("accel_pipeline.gates.run_gate", fake_gate)
    monkeypatch.setattr(
        "accel_pipeline.stages.host_bootstrap_critique.run_host_bootstrap_critique_agent",
        fake_critique,
    )

    out = run_host_bootstrap(
        pc,
        mc,
        engine_name="postgres",
        representative_workload="tpch_q1_aux",
        force=True,
    )
    assert out["status"] == "pass"
    assert calls["stage"] == 2
    assert calls["critique"] == 2
    assert (
        tmp_path
        / "engines"
        / "postgres"
        / "host_bootstrap"
        / "host_bootstrap_critique_feedback_round1.md"
    ).is_file()
