from __future__ import annotations

import json
from types import SimpleNamespace

import pytest

from accel_pipeline.failure import Failure, StageFailure
from accel_pipeline.stages import analysis as stage


@pytest.fixture
def harness(tmp_path, monkeypatch):
    """The analysis stage with agents scripted: `critiques` are what successive critic runs write."""
    path = tmp_path / "analysis.json"
    path.write_text(json.dumps({"correctness_facts": {"state_design": {}}}))
    calls, critiques, events = [], [], []

    def fake_agent(*, stage, user_message, extra_context=None, session_role=None, **_):
        calls.append((stage, session_role, user_message))
        if stage == "analysis_critic":
            report = critiques.pop(0)
            if isinstance(report, Exception):
                raise report
            (tmp_path / "analysis_critique.json").write_text(json.dumps(report))
        return "result"

    monkeypatch.setattr(stage, "run_agent_stage", fake_agent)
    monkeypatch.setattr(stage, "_ensure_dataset", lambda *a: None)
    monkeypatch.setattr(stage, "load_run_workload", lambda *a: {"workload_kind": "aux_state", **harness_workload})
    monkeypatch.setattr(stage, "analysis_artifact_path", lambda *a, **k: path)
    invalid: list[str] = []

    def fake_validate(*_):
        if invalid:
            raise RuntimeError(invalid.pop(0))

    monkeypatch.setattr(stage, "_validate", fake_validate)
    monkeypatch.setattr(stage, "log_analysis_candidates", lambda *a, **k: None)
    monkeypatch.setattr(stage, "log_event", lambda *a, **k: events.append(k))
    harness_workload: dict = {}
    harness = SimpleNamespace(dir=tmp_path)

    def run(**workload):
        harness_workload.clear()
        harness_workload.update(workload)
        stage.run(SimpleNamespace(repo_root=harness.dir), object(), "w", 3)
        return [(s, role) for s, role, _ in calls], calls

    run.critiques, run.events, run.dir, run.invalid = critiques, events, tmp_path, invalid
    return run


def test_accepted_analysis_goes_straight_on(harness):
    harness.critiques.append({"verdict": "accept", "checks": [{"id": "R1", "ok": True}]})
    stages, _ = harness()
    assert stages == [("analysis", "main"), ("analysis_critic", None)]
    assert (harness.dir / "analysis_critique_r1.json").is_file()


def test_failed_check_sends_the_critique_back_to_the_main_session(harness):
    harness.critiques.append({"verdict": "accept", "checks": [{"id": "R1", "ok": False, "case": "global window"}]})
    stages, calls = harness()
    assert stages == [("analysis", "main"), ("analysis_critic", None), ("analysis", "main")]
    assert "global window" in calls[-1][2]


def test_a_better_alternative_is_a_revision(harness):
    harness.critiques.append({"verdict": "accept", "checks": [], "alternative": {"better": True}})
    stages, _ = harness()
    assert stages[-1] == ("analysis", "main")


def test_rounds_are_capped_and_zero_disables(harness):
    revise = {"verdict": "revise", "checks": []}
    harness.critiques.extend([revise, revise])
    stages, _ = harness(analysis_critique_max_rounds=2)
    assert stages.count(("analysis_critic", None)) == 2 and stages.count(("analysis", "main")) == 3
    stages, _ = harness(analysis_critique_max_rounds=0)
    assert ("analysis_critic", None) not in stages[-1:]


def test_a_critic_that_fails_never_stops_the_run(harness):
    harness.critiques.append(StageFailure(Failure(kind="infra", stage="analysis_critic", source="codex_exec", summary="timeout")))
    stages, _ = harness()
    assert stages == [("analysis", "main"), ("analysis_critic", None)]


def test_validation_errors_go_back_to_the_main_session(harness):
    harness.invalid.append("state_storage is required")
    harness.critiques.append({"verdict": "accept", "checks": []})
    stages, calls = harness()
    assert stages == [("analysis", "main"), ("analysis", "main"), ("analysis_critic", None)]
    assert "state_storage is required" in calls[1][2]


def test_validation_repair_is_bounded(harness):
    harness.invalid.extend(["bad"] * 3)
    with pytest.raises(RuntimeError, match="bad"):
        harness()
