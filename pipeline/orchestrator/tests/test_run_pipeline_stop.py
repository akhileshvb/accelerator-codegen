import json

import pytest

from accel_pipeline import run_pipeline
from accel_pipeline.config import PipelineConfig, RepairConfig
from accel_pipeline.repair import EXIT_STOPPED, StageStopped
from accel_pipeline.state import PipelineState


@pytest.fixture
def config(tmp_path, monkeypatch):
    monkeypatch.setattr("accel_pipeline.repair.load_run_workload", lambda cfg, name: {"benchmark_name": "tpch"})
    path = tmp_path / "pipeline/artifacts/tpch/q3/pipeline_state.json"
    path.parent.mkdir(parents=True)
    PipelineState(query=3, workload="w", engine="e", stage="implementation").save(path)
    monkeypatch.setattr(run_pipeline, "state_path", lambda *a: path)
    return PipelineConfig(repo_root=tmp_path, pipeline_root=tmp_path / "pipeline", build_dir=tmp_path / "b",
                          tpch_db_sf1=tmp_path, tpch_db_sf10=tmp_path, repair=RepairConfig(max_orchestrator_retries=1))


def test_a_stage_exception_is_retried_once_then_recorded_as_a_stop(config, monkeypatch, tmp_path):
    calls = []

    def boom(*args, **kwargs):
        calls.append(kwargs["resume"])
        raise ValueError("bug")

    monkeypatch.setattr(run_pipeline, "_run_stage_once", boom)
    with pytest.raises(StageStopped, match="orchestrator exception"):
        run_pipeline._run_stage("implementation", config, None, "w", 3, {}, False)
    assert calls == [False, True]
    state = json.loads((tmp_path / "pipeline/artifacts/tpch/q3/pipeline_state.json").read_text())
    assert state["status"].startswith("stopped:") and state["stage"] == "implementation"
    assert "ValueError" in (tmp_path / "pipeline/artifacts/tpch/q3/stop_report.md").read_text()


def test_a_stage_that_recovers_on_retry_is_not_a_stop(config, monkeypatch):
    outcomes = iter([ValueError("flaky"), None])

    def once(*args, **kwargs):
        outcome = next(outcomes)
        if outcome:
            raise outcome

    monkeypatch.setattr(run_pipeline, "_run_stage_once", once)
    run_pipeline._run_stage("implementation", config, None, "w", 3, {}, False)


def test_dry_run_exceptions_still_surface(config, monkeypatch):
    monkeypatch.setattr(run_pipeline, "_run_stage_once", lambda *a, **k: (_ for _ in ()).throw(ValueError("x")))
    with pytest.raises(ValueError):
        run_pipeline._run_stage("implementation", config, None, "w", 3, {}, True)


def test_main_exits_with_code_3_on_a_stop_and_on_a_stray_exception(monkeypatch, tmp_path):
    def stopped(argv):
        raise StageStopped("gate failed", tmp_path / "stop_report.md")

    monkeypatch.setattr(run_pipeline, "_main", stopped)
    assert run_pipeline.main([]) == EXIT_STOPPED == 3

    def stray(argv):
        raise KeyError("x")

    monkeypatch.setattr(run_pipeline, "_main", stray)
    assert run_pipeline.main([]) == 3
