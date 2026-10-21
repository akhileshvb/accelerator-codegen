"""Tests for one-shot host_bootstrap stage."""

from __future__ import annotations

from dataclasses import replace
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


@pytest.mark.parametrize("engine", ["duckdb", "postgres", "mysql"])
def test_every_engine_needs_host_bootstrap(engine):
    assert needs_host_bootstrap(load_workload("tpch_q3_aux", REPO_ROOT, engine=engine)) is True


def _passing_status(tmp_path, monkeypatch, gate_data):
    def _fake_dir(repo_root: Path, engine_name: str) -> Path:
        path = tmp_path / "engines" / engine_name / "host_bootstrap"
        path.mkdir(parents=True, exist_ok=True)
        return path

    monkeypatch.setattr("accel_pipeline.host_bootstrap.host_bootstrap_dir", _fake_dir)
    write_host_bootstrap_status(tmp_path, "postgres", status="pass", extra={"gate_data": gate_data})


def _write_differential_source(tmp_path):
    source = tmp_path / "backends" / "postgres" / "tests" / "harness" / "test_postgres_tpch_differential.cpp"
    source.parent.mkdir(parents=True)
    source.write_text("// harness\n", encoding="utf-8")


def test_passed_needs_a_green_gate_result_and_the_harness_source(tmp_path, monkeypatch):
    _passing_status(tmp_path, monkeypatch, {"status": "pass", "smokes": [{"target": "t", "exit_code": 0}]})
    assert host_bootstrap_passed(tmp_path, "postgres") is False  # source missing
    _write_differential_source(tmp_path)
    assert host_bootstrap_passed(tmp_path, "postgres") is True


def test_a_marker_without_a_gate_result_is_not_a_pass(tmp_path, monkeypatch):
    _passing_status(tmp_path, monkeypatch, {})
    _write_differential_source(tmp_path)
    assert host_bootstrap_passed(tmp_path, "postgres") is False


def test_engines_needing_bootstrap_lists_every_engine_once():
    runs = [("tpch_q3_aux", "duckdb"), ("tpch_q3_aux", "postgres"), ("tpch_q1_aux", "postgres"), ("tpch_q1_aux", "mysql")]
    needed = engines_needing_bootstrap_from_workloads(REPO_ROOT, runs)
    assert sorted(e for e, _ in needed) == ["duckdb", "mysql", "postgres"]


def test_host_bootstrap_fails_before_the_backend_exists(tmp_path):
    pc = replace(load_pipeline_config(repo_root=REPO_ROOT), repo_root=tmp_path)
    with pytest.raises(GateFailure, match="engine_backend"):
        run_host_bootstrap(pc, load_model_config(repo_root=REPO_ROOT), engine_name="postgres",
                           representative_workload="tpch_q1_aux", force=True)


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


def test_host_bootstrap_gate_builds_all_five_harnesses_and_smokes_them():
    text = (REPO_ROOT / "pipeline" / "gates" / "run_host_bootstrap.sh").read_text(encoding="utf-8")
    for var in ("DIFF_BINARY", "MULTI_CONN_TARGET", "CROSS_ACCEL_TARGET", "HARNESS_BENCH_TARGET", "HARNESS_CALIBRATE_TARGET"):
        assert f"${{{var}}}" in text
    assert "--help" in text and "check_backend_query_free.py" in text


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


def _hb_write(path, engine="postgres", stage="host_bootstrap"):
    from accel_pipeline.write_ownership import write_allowed

    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("tpch_q1_aux", REPO_ROOT, engine=engine)
    from accel_pipeline.config import apply_workload_overrides

    merged = apply_workload_overrides(pc, w)
    return write_allowed(REPO_ROOT, path, query=1, stage=stage, write_paths=merged.write_paths, pipeline_config=merged)


@pytest.mark.parametrize(
    "path",
    [
        "backends/postgres/tests/harness/test_postgres_tpch_differential.cpp",
        "backends/postgres/bench/benchmark_postgres_tpch_aux.cpp",
        "backends/postgres/tests/harness.cmake",
    ],
)
def test_host_bootstrap_writes_its_engines_harnesses(path):
    assert _hb_write(path)[0] is True


@pytest.mark.parametrize(
    "path",
    [
        "backends/postgres/engine/store.cpp",
        "backends/postgres/api/register.hpp",
        "backends/postgres/backend.cmake",
        "backends/postgres/tests/test_postgres_writer.cpp",
        "backends/mysql/tests/harness/test_mysql_tpch_differential.cpp",
        "accelerators/tpch/q1/postgres/aux/module.cpp",
        "substrate/aux/core/aux_types.hpp",
    ],
)
def test_host_bootstrap_cannot_write_anything_else(path):
    assert _hb_write(path)[0] is False


def test_other_stages_still_cannot_write_a_backend_harness():
    assert _hb_write("backends/postgres/tests/harness/x.cpp", stage="implementation")[0] is False


def test_a_recorded_harness_defect_reopens_the_bootstrap(tmp_path):
    import os
    import time

    from accel_pipeline.host_bootstrap import (
        backend_additions_path,
        host_bootstrap_status_path,
        pending_harness_requests,
    )

    status = host_bootstrap_status_path(tmp_path, "anyengine")
    status.parent.mkdir(parents=True, exist_ok=True)
    status.write_text('{"status": "pass"}')
    assert pending_harness_requests(tmp_path, "anyengine") == ""

    requests = backend_additions_path(tmp_path, "anyengine")
    requests.parent.mkdir(parents=True, exist_ok=True)
    requests.write_text('{"helper": "transaction-safe setup"}\n')
    later = time.time() + 5
    os.utime(requests, (later, later))
    assert "transaction-safe setup" in pending_harness_requests(tmp_path, "anyengine")
