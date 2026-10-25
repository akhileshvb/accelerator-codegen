"""The engine_backend flow under the repair loop, with a recording fake agent: one main session across
layers and repairs (only the new feedback sent), the critic's batched findings, validation failures
routed to the owning layer, clean stops, and relaunch from the markers."""

from __future__ import annotations

import json
import shutil
from pathlib import Path

import pytest

from accel_pipeline import repair as repair_mod
from accel_pipeline import session as session_mod
from accel_pipeline.backends.base import StageRunRequest, StageRunResult
from accel_pipeline.config import load_model_config, load_pipeline_config, load_workload
from accel_pipeline.failure import StageFailure, Failure
from accel_pipeline.gates.run import GateResult
from accel_pipeline.repair import StageStopped
from accel_pipeline.session import run_in_session
from accel_pipeline.engine_backend import stage as eb
from accel_pipeline.engine_backend.progress import Progress

REAL_PIPELINE = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).parent / "fixtures"


class FakeBackend:
    name = "fake"

    def __init__(self, home: Path) -> None:
        self.home, self.requests, self.counter = home, [], 0

    def codex_home(self) -> Path:
        return self.home

    def run_stage(self, request):
        resumed = request.metadata.get("resume_session_id")
        sid = resumed or f"sess-{self.counter}"
        self.counter += 0 if resumed else 1
        self.requests.append((request, sid, bool(resumed)))
        (self.home / "sessions").mkdir(parents=True, exist_ok=True)
        (self.home / "sessions" / f"rollout-x-{sid}.jsonl").write_text("")
        return StageRunResult(backend="fake", stage=request.stage, status="finished", metadata={"session_id": sid})


class World:
    def __init__(self, tmp_path, monkeypatch, **workload):
        self.art = tmp_path / "art"
        self.art.mkdir()
        repo = tmp_path / "repo"
        for sub in ("config", "schemas", "prompts", "templates"):
            shutil.copytree(REAL_PIPELINE / sub, repo / "pipeline" / sub)
        shutil.copy(FIXTURES / "engine_toydb.yaml", repo / "pipeline" / "config" / "workloads")
        self.pc = load_pipeline_config(repo_root=repo)
        self.mc = load_model_config(repo_root=REAL_PIPELINE.parents[0])
        self.workload = load_workload("engine_toydb", repo)
        self.workload.update(workload)
        self.backend = FakeBackend(tmp_path / "home")
        self.agent = []  # (stage, role, message)
        self.gates = []  # scripted failures by call; a gate name or None
        self.gate_calls = []
        self.critic_calls = []
        self.critic_script = []
        self.events = []
        for module in (eb, repair_mod):
            monkeypatch.setattr(module, "query_artifacts_dir", lambda *a, **k: self.art)
            monkeypatch.setattr(module, "append_experiment_log", lambda *a, **k: None)
            monkeypatch.setattr(module, "load_run_workload", lambda cfg, name: self.workload)
        monkeypatch.setattr(eb, "run_agent_stage", self._agent)
        monkeypatch.setattr(eb, "run_post_stage_gates_collect", self._gates)
        monkeypatch.setattr(eb, "run_layer_critique_agent", self._critic)
        monkeypatch.setattr(eb, "snapshot_backend", lambda *a, **k: None)
        monkeypatch.setattr(eb, "backend_diff", lambda *a, **k: "diff --git a/x b/x")
        monkeypatch.setattr(eb, "backend_digest", lambda *a, **k: "## Backend digest\n\n")
        monkeypatch.setattr(repair_mod.time, "sleep", lambda s: None)
        monkeypatch.setattr("accel_pipeline.failure.handle_infra_failure", lambda *a, **k: None)
        monkeypatch.setattr(repair_mod, "_rollback", lambda ctx: None)
        self.monkeypatch = monkeypatch

    def _agent(self, **kw):
        request = StageRunRequest(stage=kw["stage"], instructions="I", user_message=kw["user_message"],
                                  agent_name="a", model_id="m")
        self.agent.append((kw["stage"], kw["session_role"], kw["user_message"]))
        result = run_in_session(self.backend, request, pipeline_config=kw["pipeline_config"], art=self.art, query=0,
                                role=kw["session_role"], handoff=kw.get("handoff"))
        return result

    def _gates(self, pc, workload_name, query, names, **kw):
        self.gate_calls.append(list(names))
        results = []
        for name in names:
            if self.gates and self.gates[0] == name:
                self.gates.pop(0)
                failed = GateResult(name, "fail", {"log_tail": f"{name} assertion failed in backends/toydb/engine/x.cpp"})
                return results + [failed], failed
            results.append(GateResult(name, "pass", {}))
        return results, None

    def _critic(self, pc, mc, workload_name, query, overrides, dry_run, layer, *, round_idx, extra_context, brief=""):
        self.critic_calls.append((layer["id"], round_idx, brief))
        self.events.append(f"critic:{layer['id']}:{round_idx}")
        report = self.critic_script.pop(0) if self.critic_script else {"status": "pass", "violations": []}
        (self.art / f"layer_critique_{layer['id']}.json").write_text(json.dumps({"layer": layer["id"], **report}))

    def layer(self, lid):
        return next(l for l in eb.implementation_layers(self.workload) if l["id"] == lid)

    def run_layer(self, lid, **kw):
        return eb.run_implementation_layer(self.pc, self.mc, "engine_toydb", 0, {}, False, self.layer(lid), **kw)

    def sessions(self):
        return {req.stage: (sid, resumed) for req, sid, resumed in self.backend.requests}


@pytest.fixture
def world(tmp_path, monkeypatch):
    return World(tmp_path, monkeypatch)


def test_a_gate_failure_resumes_the_same_session_with_only_the_feedback(world):
    world.gates = ["engine_capture"]
    world.run_layer("capture")
    first, repair = world.agent
    assert first[0] == repair[0] == "impl_capture" and first[1] == repair[1] == "main"
    assert "Engine backend stage=impl_capture" in first[2] and "Backend digest" in first[2]
    assert "engine_capture assertion failed" in repair[2] and "Engine backend stage" not in repair[2]
    (_, sid1, resumed1), (_, sid2, resumed2) = [(r.stage, s, x) for r, s, x in world.backend.requests]
    assert sid1 == sid2 and not resumed1 and resumed2
    assert Progress(world.art).layer_passed("capture")
    assert not (world.art / "layer_capture.feedback.md").exists()


def test_the_next_layer_continues_the_session_with_its_own_first_message(world):
    world.run_layer("hostctl")
    world.run_layer("capture")
    (req1, sid1, _), (req2, sid2, resumed2) = world.backend.requests
    assert sid1 == sid2 and resumed2
    assert req2.metadata["phase_change"] is True  # the capture layer's instructions are sent once
    assert "Engine backend stage=impl_capture" in req2.user_message
    assert "Backend digest" not in req2.user_message  # the session already knows the backend


def test_past_the_context_cap_a_fresh_session_starts_from_the_digest(world, monkeypatch):
    world.workload["main_session_max_context_tokens"] = 1000
    monkeypatch.setattr(session_mod, "context_tokens", lambda home, sid: 5000)
    world.run_layer("hostctl")
    world.run_layer("capture")
    (_, sid1, _), (req2, sid2, resumed2) = world.backend.requests
    assert sid1 != sid2 and not resumed2
    assert req2.user_message.startswith("## Backend digest")


def test_gate_exhaustion_is_a_clean_stop_with_a_report_not_an_exception(world):
    world.gates = ["engine_capture"] * 10
    with pytest.raises(StageStopped) as stopped:
        world.run_layer("capture")
    assert "engine_capture" in Path(stopped.value.report).read_text()
    assert len(world.agent) == 1 + world.workload["layer_gate_max_rounds"]
    assert not Progress(world.art).layer_passed("capture")
    assert (world.art / "layer_capture.feedback.md").is_file()


def test_a_relaunch_continues_from_the_feedback_the_stop_left(world):
    world.gates = ["engine_capture"] * 10
    with pytest.raises(StageStopped):
        world.run_layer("capture")
    world.agent.clear()
    world.gates = []
    world.run_layer("capture")
    assert "Carried over" in world.agent[0][2] and "engine_capture assertion failed" in world.agent[0][2]
    assert Progress(world.art).layer_passed("capture")


def test_finished_layers_are_not_redone_on_a_relaunch(world):
    world.run_layer("hostctl")
    world.agent.clear()
    eb.run_implementation(world.pc, world.mc, "engine_toydb", 0, {}, False)
    assert [stage for stage, _, _ in world.agent][0] == "impl_capture"
    assert all(stage != "impl_hostctl" for stage, _, _ in world.agent)


def test_the_critic_starts_when_the_build_is_green_and_its_findings_come_back_as_one_round(world):
    world.critic_script = [
        {"status": "fail", "summary": "s", "feedback_for_implementation": "batch",
         "violations": [
             {"rule": "contract", "severity": "blocking", "kind": "code_fix", "location": "backends/toydb/a.cpp:10",
              "contract_item": "lock discipline", "expected": "no engine call under the lock", "evidence": "e1"},
             {"rule": "contract", "severity": "blocking", "kind": "code_fix", "location": "backends/toydb/b.cpp:20",
              "contract_item": "bounded waits", "expected": "the wait fails at its bound", "evidence": "e2"},
             {"rule": "harness_gap", "severity": "blocking", "kind": "needs_canary", "contract_item": "ddl events",
              "expected": "a replaced table is never answered stale", "evidence": "e3"}]},
        {"status": "pass", "violations": []},
    ]
    world.run_layer("capture")
    repair = world.agent[1]
    assert repair[1] == "main" and "a.cpp:10" in repair[2] and "b.cpp:20" in repair[2] and "ddl events" not in repair[2]
    assert "no engine call under the lock" in repair[2]  # expected behaviour, not just a complaint
    assert [(lid, rnd) for lid, rnd, _ in world.critic_calls] == [("capture", 1), ("capture", 2)]
    assert "diff --git" in world.critic_calls[0][2] and "a.cpp:10" in world.critic_calls[1][2]  # re-check: only those
    assert "ddl events" in (world.art / "canary_requests.md").read_text()
    assert len(world.agent) == 2  # one batched repair round, not one per finding
    # the build gate came first, the critic joined after the layer's gates passed
    assert world.gate_calls[0] == ["engine_build"]


def test_a_critic_that_never_writes_valid_json_is_recorded_not_raised(world):
    world.critic_script = [{"status": "maybe"}]
    world.run_layer("capture")
    report = json.loads((world.art / "layer_critique_capture.json").read_text())
    assert report["status"] == "error" and "unavailable" in report["summary"]
    assert Progress(world.art).layer_passed("capture")


def test_a_critic_that_keeps_failing_leaves_open_findings_for_the_audit(world):
    blocking = {"status": "fail", "feedback_for_implementation": "f",
                "violations": [{"rule": "contract", "severity": "blocking", "kind": "code_fix", "evidence": "still"}]}
    world.critic_script = [blocking, blocking]
    world.run_layer("capture")
    assert "still" in (world.art / "layer_capture.open_findings.md").read_text()
    assert Progress(world.art).layer_passed("capture")


def test_the_audit_sends_each_owning_layer_its_violations_in_one_round(world):
    from accel_pipeline.engine_backend import architecture_audit as audit_mod

    report = {"status": "fail", "summary": "s", "feedback_for_implementation": "x",
              "violations": [{"rule": "slots_only", "layer": "capture", "evidence": "c1"},
                             {"rule": "slots_only", "layer": "capture", "evidence": "c2"},
                             {"rule": "fail_closed", "layer": "scan", "evidence": "s1"},
                             {"rule": "independence", "evidence": "nowhere"}]}
    audited = []

    def fake_audit_agent(*args, **kw):
        audited.append(kw["round_idx"])
        (world.art / "architecture_audit.json").write_text(json.dumps(report))

    world.monkeypatch.setattr(audit_mod, "run_architecture_audit_agent", fake_audit_agent)
    world.monkeypatch.setattr(audit_mod, "query_artifacts_dir", lambda *a, **k: world.art)
    world.monkeypatch.setattr(audit_mod, "append_experiment_log", lambda *a, **k: None)
    world.monkeypatch.setattr(audit_mod, "load_run_workload", lambda cfg, name: world.workload)
    eb._audit(world.pc, world.mc, "engine_toydb", 0, {}, False)
    stages = [stage for stage, _, _ in world.agent]
    assert audited == [1]  # one audit pass
    assert stages == ["impl_capture", "impl_scan", "impl_api"]  # per layer, in order; unnamed -> the last layer
    messages = {stage: msg for stage, _, msg in world.agent}
    assert "c1" in messages["impl_capture"] and "c2" in messages["impl_capture"] and "s1" not in messages["impl_capture"]
    assert "nowhere" in messages["impl_api"]
    assert Progress(world.art).done("architecture_audit")


def test_validation_failure_goes_to_the_owning_layer_then_the_gate_reruns(world):
    world.workload["post_validation_gates"] = ["engine_build", "slot_txn_edges", "slot_tsan"]
    world.gates = ["slot_txn_edges"]
    eb.run_validation(world.pc, world.mc, "engine_toydb", 0, {}, False)
    stages = [(stage, role) for stage, role, _ in world.agent]
    assert stages == [("validation", "validation"), ("impl_capture", "main")]  # slot_txn_edges is owned by capture
    assert "slot_txn_edges assertion failed" in world.agent[1][2]
    # after the fix the owner's gates, then the failing gate and the ones after it ran again
    flat = [g for call in world.gate_calls for g in call]
    assert flat.count("slot_txn_edges") == 2 and flat.count("slot_tsan") >= 1
    assert Progress(world.art).gate_passed("slot_tsan")


def test_a_gate_may_name_its_owner_in_its_own_report(world):
    workload = world.workload
    assert eb.validation_owner("engine_canary", {"owner_layer": "scan"}, workload) == "scan"
    assert eb.validation_owner("engine_canary", {}, workload) == "api"
    assert eb.validation_owner("engine_adversarial", {}, workload) == "validation"
    assert eb.validation_owner("engine_capture", {}, workload) == "capture"
    assert eb.validation_owner("unknown_gate", {}, workload) == "api"


def test_a_validation_that_cannot_be_repaired_stops_cleanly(world):
    world.workload["post_validation_gates"] = ["slot_txn_edges"]
    world.gates = ["slot_txn_edges"] * 50
    with pytest.raises(StageStopped):
        eb.run_validation(world.pc, world.mc, "engine_toydb", 0, {}, False)
    assert not Progress(world.art).gate_passed("slot_txn_edges")


def test_an_agent_that_times_out_is_resumed_not_restarted(world):
    outcomes = [StageFailure(Failure(kind="timeout", stage="impl_capture", source="agent", summary="deadline")), None]
    real = world._agent

    def flaky(**kw):
        outcome = outcomes.pop(0)
        result = real(**kw)
        if outcome:
            raise outcome
        return result

    world.monkeypatch.setattr(eb, "run_agent_stage", flaky)
    world.run_layer("capture")
    assert len(world.agent) == 2
    (_, sid1, _), (_, sid2, resumed2) = [(r.stage, s, x) for r, s, x in world.backend.requests]
    assert sid1 == sid2 and resumed2
    assert "deadline" in world.agent[1][2]


def test_skip_gates_runs_the_turn_alone(world):
    world.run_layer("capture", skip_gates=True)
    assert world.gate_calls == [] and world.critic_calls == []
    assert Progress(world.art).layer_passed("capture")


def test_analysis_that_fails_is_resumed_then_marked_done_and_skipped_on_a_relaunch(world):
    outcomes = [StageFailure(Failure(kind="timeout", stage="analysis", source="agent", summary="deadline")), None]
    real = world._agent

    def flaky(**kw):
        outcome = outcomes.pop(0)
        result = real(**kw)
        if outcome:
            raise outcome
        return result

    world.monkeypatch.setattr(eb, "run_agent_stage", flaky)
    eb.run_analysis(world.pc, world.mc, "engine_toydb", 0, {}, False)
    assert [role for _, role, _ in world.agent] == ["analysis", "analysis"] and "deadline" in world.agent[1][2]
    eb.run_analysis(world.pc, world.mc, "engine_toydb", 0, {}, False)
    assert len(world.agent) == 2  # done: not run again
