"""The engine-backend pipeline: its gates and write rules, the layered flow under the repair loop with a recording
fake agent (one main session across layers and repairs, the critics' batched findings, validation failures routed to
the owning layer, clean stops, relaunch from the markers), critics as data, the backend digest, backend requests and
host_bootstrap with its harness acceptance."""

from __future__ import annotations

import json
import os
import time
import shutil
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace

import pytest

from accel_pipeline import agent as agent_mod, engine_backend as eb, repair as repair_mod
from accel_pipeline.agent import StageRunRequest, StageRunResult, run_in_session
from accel_pipeline.config import (
    apply_workload_overrides,
    harness_targets,
    load_model_config,
    load_pipeline_config,
    load_workload,
)
from accel_pipeline.engine_backend import (
    Progress,
    architecture_critic,
    backend_diff,
    backend_digest,
    host_bootstrap_critic,
    host_bootstrap_passed,
    implementation_layers,
    layer_contracts_digest,
    layer_critic,
    load_verdict,
    run_engine_pipeline,
    run_host_bootstrap,
    snapshot_backend,
    verdict_feedback,
    write_host_bootstrap_status,
)
from accel_pipeline.gates import GateResult
from accel_pipeline.layout import write_allowed
from accel_pipeline.repair import Failure, StageFailure, StageStopped

REPO_ROOT = Path(__file__).resolve().parents[3]
REAL_PIPELINE = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).parent / "fixtures"


def test_backend_stage_gates_are_wired():
    from accel_pipeline.gates import GATE_SCRIPTS

    gates_dir = REPO_ROOT / "pipeline" / "gates"
    for engine in ("engine_mysql", "engine_postgres", "engine_duckdb"):
        w = load_workload(engine, REPO_ROOT)
        wired = list(w["post_validation_gates"]) + [layer["gate"] for layer in w["implementation_layers"]]
        for gate in wired:
            assert gate in GATE_SCRIPTS, gate
            assert (gates_dir / GATE_SCRIPTS[gate]).is_file(), gate
    slot_gates = [g for g in GATE_SCRIPTS if g.startswith("slot_")]
    assert len(slot_gates) == 8  # the differential runs in the accelerator pipeline (aux_correctness)
    assert not (gates_dir / "slot_adapters").exists()


def test_engine_write_paths_allow_adapter_edits():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("engine_mysql", REPO_ROOT)
    merged = apply_workload_overrides(pc, w)
    ok, reason = write_allowed(
        REPO_ROOT,
        "backends/mysql/engine/scan_helper.hpp",
        query=0,
        stage="impl_scan",
        write_paths=merged.write_paths,
        pipeline_config=merged,
        benchmark="engine",
    )
    assert ok, reason
    ok2, reason2 = write_allowed(
        REPO_ROOT,
        "backends/mysql/api/mysql_host.cpp",
        query=0,
        stage="impl_runtime",
        write_paths=merged.write_paths,
        pipeline_config=merged,
        benchmark="engine",
    )
    assert ok2, reason2
    ok3, reason3 = write_allowed(
        REPO_ROOT,
        "backends/mysql/tests/test_mysql_capture.cpp",
        query=0,
        stage="impl_capture",
        write_paths=merged.write_paths,
        pipeline_config=merged,
        benchmark="engine",
    )
    assert ok3, reason3
    ok4, reason4 = write_allowed(
        REPO_ROOT,
        "backends/mysql/api/catalog/query_catalog.hpp",
        query=0,
        stage="impl_runtime",
        write_paths=merged.write_paths,
        pipeline_config=merged,
        benchmark="engine",
    )
    assert ok4, reason4
    # another engine's backend, the substrate and query directories stay closed
    for rel in ("backends/duckdb/api/register.cpp", "substrate/aux/core/aux_types.hpp",
                "accelerators/tpch/q6/mysql/aux/module.cpp", "workloads/tpch/q6.sql"):
        ok5, reason5 = write_allowed(
            REPO_ROOT, rel, query=0, stage="impl_runtime", write_paths=merged.write_paths + ["**"],
            pipeline_config=merged, benchmark="engine",
        )
        assert not ok5, (rel, reason5)


def test_critique_capture_cannot_write_adapter():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("engine_postgres", REPO_ROOT)
    merged = apply_workload_overrides(pc, w)
    ok, reason = write_allowed(
        REPO_ROOT,
        "backends/postgres/api/postgres_host.cpp",
        query=0,
        stage="critique_capture",
        write_paths=merged.write_paths,
        pipeline_config=merged,
        benchmark="engine",
    )
    assert not ok
    assert "pipeline/artifacts" in reason
    ok2, reason2 = write_allowed(
        REPO_ROOT,
        "pipeline/artifacts/engines/postgres/layer_critique_capture.json",
        query=0,
        stage="critique_capture",
        write_paths=merged.write_paths,
        pipeline_config=merged,
        benchmark="engine",
    )
    assert ok2, reason2


def test_engine_pipeline_dry_run():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    mc = load_model_config(repo_root=REPO_ROOT)
    run_engine_pipeline(pc, mc, "engine_mysql", 0, {}, dry_run=True, stage="all", skip_gates=True)


def test_embedded_engine_needs_no_docker_host():
    from accel_pipeline.engine_backend import _layer_should_run

    w = load_workload("engine_duckdb", REPO_ROOT)
    assert w["host_runtime"]["kind"] == "host_cmake"
    host_layer = next(layer for layer in implementation_layers(w) if layer["id"] == "host")
    assert not _layer_should_run(w, host_layer)
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    mc = load_model_config(repo_root=REPO_ROOT)
    run_engine_pipeline(pc, mc, "engine_duckdb", 0, {}, dry_run=True, stage="all", skip_gates=True)


def test_run_until_artifact_retries_then_succeeds(tmp_path):
    from accel_pipeline.engine_backend import run_until_artifact

    path = tmp_path / "layer_critique_epochs.json"
    calls: list[str] = []

    def invoke(extra: str):
        calls.append(extra)
        if len(calls) == 2:
            path.write_text('{"status":"pass"}\n', encoding="utf-8")
        return object()

    run_until_artifact(artifact=path, invoke=invoke, attempts=2)
    assert len(calls) == 2
    assert calls[0] == ""
    assert "STOP EXPLORING" in calls[1]
    assert path.is_file()


def test_run_until_artifact_retries_on_invalid_json(tmp_path):
    from accel_pipeline.engine_backend import run_until_artifact

    path = tmp_path / "layer_critique_api.json"
    calls: list[str] = []

    def invoke(extra: str):
        calls.append(extra)
        path.write_text('{"status":"fail"}\n  }\n' if len(calls) == 1 else '{"status":"fail"}\n',
                        encoding="utf-8")
        return object()

    run_until_artifact(artifact=path, invoke=invoke, attempts=2)
    assert len(calls) == 2
    assert "not valid JSON" in calls[1]


def _layer_critic(tmp_path, layer="capture"):
    return layer_critic(tmp_path, layer, {})


def test_layer_critique_feedback_and_load(tmp_path):
    critic = _layer_critic(tmp_path)
    missing = load_verdict(critic)  # recorded, never raised
    assert missing["status"] == "error" and "unavailable" in missing["summary"]
    assert json.loads(critic.verdict.read_text())["status"] == "error"
    critic.verdict.write_text(
        json.dumps(
            {
                "engine_name": "postgres",
                "layer": "capture",
                "status": "FAIL",
                "summary": "Invalidate uses caller partition ids",
                "violations": [
                    {
                        "rule": "harness_gap",
                        "evidence": "TryPublish remaps partitions; harness only tests InvalidateAll",
                        "paths": ["backends/postgres/api/postgres_host.cpp"],
                    }
                ],
                "feedback_for_implementation": "Assert Invalidate(PartitionSet) after remap.",
            }
        ),
        encoding="utf-8",
    )
    verdict = load_verdict(critic)
    assert verdict["status"] == "fail"
    text = verdict_feedback(critic, verdict, 1)
    assert "Layer critique FAILED" in text
    assert "harness_gap" in text
    assert "Invalidate(PartitionSet)" in text


def test_critique_harness_gaps_aimed_at_the_pipeline_are_dropped(tmp_path):
    critic = _layer_critic(tmp_path, "hostctl")

    def critique(violations):
        critic.verdict.write_text(json.dumps({"status": "fail", "layer": "hostctl", "violations": violations}),
                                  encoding="utf-8")
        return load_verdict(critic)

    gate_gap = {"rule": "harness_gap", "paths": ["pipeline/gates/run_engine_hostctl.sh", "backends/x/host.sh"]}
    real = {"rule": "host_contract", "paths": ["backends/x/host.sh", "pipeline/prompts/engine_backend/host_contract.md"]}
    only_gate = critique([gate_gap])
    assert only_gate["status"] == "pass" and only_gate["dropped_pipeline_harness_gaps"] == [gate_gap]
    mixed = critique([gate_gap, real])
    assert mixed["status"] == "fail" and mixed["violations"] == [real]


def test_a_critic_is_data_each_with_its_own_verdict_and_rounds(tmp_path):
    audit, bootstrap = architecture_critic(tmp_path), host_bootstrap_critic(tmp_path / "hb", tmp_path)
    layer = _layer_critic(tmp_path)
    assert {c.stage for c in (audit, bootstrap, layer)} == {"architecture_audit", "critique_host_bootstrap", "critique_capture"}
    assert (layer.rounds({}), audit.rounds({}), bootstrap.rounds({})) == (0, 1, 1)
    assert layer.rounds({"layer_critique_max_rounds": 2}) == 2
    assert bootstrap.context == {"bootstrap_artifacts_path": "hb"}
    # the audit sends a layer only the violations it owns
    report = {"status": "fail", "violations": [{"rule": "a", "layer": "capture", "evidence": "c1"},
                                               {"rule": "b", "layer": "scan", "evidence": "s1"}]}
    text = verdict_feedback(audit, report, 1, layer="capture")
    assert "c1" in text and "s1" not in text and "Architecture audit FAILED" in text


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
        monkeypatch.setattr(eb, "run_critic", self._critic)
        monkeypatch.setattr(eb, "snapshot_backend", lambda *a, **k: None)
        monkeypatch.setattr(eb, "backend_diff", lambda *a, **k: "diff --git a/x b/x")
        monkeypatch.setattr(eb, "backend_digest", lambda *a, **k: "## Backend digest\n\n")
        monkeypatch.setattr(repair_mod.time, "sleep", lambda s: None)
        monkeypatch.setattr("accel_pipeline.repair.handle_infra_failure", lambda *a, **k: None)
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

    def _critic(self, critic, pc, mc, workload_name, query, overrides, message, *, dry_run=False):
        lid = critic.layer
        round_idx = int(message.split("round=")[1].split()[0])
        self.critic_calls.append((lid, round_idx, message))
        self.events.append(f"critic:{lid}:{round_idx}")
        report = self.critic_script.pop(0) if self.critic_script else {"status": "pass", "violations": []}
        critic.verdict.write_text(json.dumps({"layer": lid, **report}))

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
    monkeypatch.setattr(agent_mod, "context_tokens", lambda home, sid: 5000)
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
    report = {"status": "fail", "summary": "s", "feedback_for_implementation": "x",
              "violations": [{"rule": "slots_only", "layer": "capture", "evidence": "c1"},
                             {"rule": "slots_only", "layer": "capture", "evidence": "c2"},
                             {"rule": "fail_closed", "layer": "scan", "evidence": "s1"},
                             {"rule": "independence", "evidence": "nowhere"}]}
    audited = []

    def fake_audit_agent(critic, pc, mc, workload_name, query, overrides, message, *, dry_run=False):
        audited.append(message)
        critic.verdict.write_text(json.dumps(report))

    world.monkeypatch.setattr(eb, "run_critic", fake_audit_agent)
    eb.run_architecture_audit(world.pc, world.mc, "engine_toydb", 0, {}, False)
    stages = [stage for stage, _, _ in world.agent]
    assert len(audited) == 1 and "round=1/1" in audited[0]  # one audit pass
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


README = """# API

## Contract roles

| role | headers | symbols |
|---|---|---|
| registration | `api/register.hpp` | `RegisterThing`, `ThingRegistrar` |
| scan | `api/scan.hpp` | `ScanPartitioned` |
| introspection | | |
"""


def make_backend(root: Path) -> None:
    api = root / "backends" / "toydb" / "api"
    api.mkdir(parents=True)
    (api / "README.md").write_text(README)
    (api / "register.hpp").write_text("// GENERATED BACKEND\n// The registrar every accelerator plugs in through.\nint RegisterThing();\n")
    (api / "scan.hpp").write_text("#pragma once\nint ScanPartitioned();\n")
    tests = root / "backends" / "toydb" / "tests"
    tests.mkdir()
    (tests / "test_toydb_scan.cpp").write_text("int main() {}\n")


def test_digest_lists_files_with_roles_api_symbols_decisions_and_the_next_layers_files(tmp_path):
    make_backend(tmp_path)
    art = tmp_path / "art"
    art.mkdir()
    (art / "experiment_log.md").write_text("- layer `capture` gate FAIL\n- layer `capture` PASS\n")
    (art / "layer_capture.open_findings.md").write_text("blocking: lock held across an engine call\n")
    (art / "backend_analysis.json").write_text('{"layers": [{"id": "epochs", "files": ["backends/toydb/engine/epochs.cpp"]}]}')
    text = backend_digest(tmp_path, "toydb", art, next_layer="epochs")
    assert "`backends/toydb/api/register.hpp`" in text and "The registrar every accelerator plugs in through." in text
    assert "`backends/toydb/tests/test_toydb_scan.cpp`" in text and "layer harness or test" in text
    assert "registration: `RegisterThing`, `ThingRegistrar`" in text and "scan: `ScanPartitioned`" in text
    assert "introspection" not in text  # an n/a role lists nothing
    assert "layer `capture` PASS" in text and "lock held across an engine call" in text
    assert "`backends/toydb/engine/epochs.cpp`" in text
    assert backend_digest(tmp_path, "toydb", art) == backend_digest(tmp_path, "toydb", art)  # deterministic


def test_digest_of_an_empty_backend_says_so(tmp_path):
    assert "(the backend tree is empty)" in backend_digest(tmp_path, "toydb", tmp_path)


def test_diff_against_a_snapshot_shows_only_what_changed(tmp_path):
    make_backend(tmp_path)
    snapshot_backend(tmp_path, "toydb", tmp_path / "before")
    assert backend_diff(tmp_path, "toydb", tmp_path / "before") == ""
    (tmp_path / "backends/toydb/api/scan.hpp").write_text("#pragma once\nint ScanPartitioned(int partitions);\n")
    (tmp_path / "backends/toydb/engine").mkdir()
    (tmp_path / "backends/toydb/engine/new.cpp").write_text("int x;\n")
    (tmp_path / "backends/toydb/tests/test_toydb_scan.cpp").unlink()
    diff = backend_diff(tmp_path, "toydb", tmp_path / "before")
    assert "+int ScanPartitioned(int partitions);" in diff and "+int x;" in diff and "-int main() {}" in diff
    assert "register.hpp" not in diff


def test_contract_digest_keeps_the_layers_section_and_the_requirements_naming_it():
    contracts = (REPO_ROOT / "pipeline/prompts/engine_backend/layer_contracts.md").read_text()
    capture = layer_contracts_digest(contracts, "capture", "layer_contracts.md")
    scan = layer_contracts_digest(contracts, "scan", "layer_contracts.md")
    assert "## `capture` (S1)" in capture and "## `scan` (S5, S8)" not in capture
    assert "**Writes are judged at commit**" in capture and "**Own writes**" not in capture  # names runtime, not capture
    assert "**Lock discipline**" in capture and "**Lock discipline**" in scan  # all layers
    assert len(capture) < len(contracts) / 2 and len(scan) < len(contracts) / 2
    assert "layer_contracts.md" in capture.splitlines()[-1]


def _passing_status(tmp_path, monkeypatch, gate_data):
    def _fake_dir(repo_root: Path, engine_name: str) -> Path:
        path = tmp_path / "engines" / engine_name / "host_bootstrap"
        path.mkdir(parents=True, exist_ok=True)
        return path

    monkeypatch.setattr("accel_pipeline.engine_backend.host_bootstrap_dir", _fake_dir)
    write_host_bootstrap_status(tmp_path, "postgres", status="pass", extra={"gate_data": gate_data})


def _write_differential_source(tmp_path):
    source = tmp_path / "backends" / "postgres" / "tests" / "harness" / "test_postgres_tpch_differential.cpp"
    source.parent.mkdir(parents=True)
    source.write_text("// harness\n", encoding="utf-8")


def _green(benchmark):
    return {"status": "pass", "smokes": [{"target": "t", "exit_code": 0}],
            "targets": list(harness_targets("postgres", benchmark).values())}


def test_passed_needs_a_green_gate_result_and_the_harness_source(tmp_path, monkeypatch):
    _passing_status(tmp_path, monkeypatch, _green("tpch"))
    assert host_bootstrap_passed(tmp_path, "postgres", "tpch") is False  # source missing
    _write_differential_source(tmp_path)
    assert host_bootstrap_passed(tmp_path, "postgres", "tpch") is True


def test_a_pass_covers_only_the_benchmark_whose_targets_it_built(tmp_path, monkeypatch):
    _passing_status(tmp_path, monkeypatch, _green("tpch"))
    _write_differential_source(tmp_path)
    assert host_bootstrap_passed(tmp_path, "postgres", "sec_edgar") is False


def test_a_marker_without_a_gate_result_is_not_a_pass(tmp_path, monkeypatch):
    _passing_status(tmp_path, monkeypatch, {})
    _write_differential_source(tmp_path)
    assert host_bootstrap_passed(tmp_path, "postgres", "tpch") is False


def test_host_bootstrap_fails_before_the_backend_exists(tmp_path):
    pc = replace(load_pipeline_config(repo_root=REPO_ROOT), repo_root=tmp_path)
    with pytest.raises(StageStopped, match="backend is missing"):
        run_host_bootstrap(pc, load_model_config(repo_root=REPO_ROOT), engine_name="postgres",
                           representative_workload="tpch_q1_aux", force=True)


def _patch_bootstrap_dirs(tmp_path, monkeypatch):
    def _fake_dir(repo_root, engine_name):
        path = tmp_path / "engines" / engine_name / "host_bootstrap"
        path.mkdir(parents=True, exist_ok=True)
        return path

    monkeypatch.setattr("accel_pipeline.engine_backend.host_bootstrap_dir", _fake_dir)
    monkeypatch.setattr("accel_pipeline.stages.materialize_registration", lambda *a: {"created": []})
    monkeypatch.setattr(
        "accel_pipeline.runlog.append_experiment_log", lambda *a, **k: tmp_path
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
        if args[1] != "host_bootstrap":
            return GateResult(name=args[1], status="pass", data={}, path=tmp_path / "other.json")
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

    monkeypatch.setattr("accel_pipeline.engine_backend.load_workload", fake_load)
    monkeypatch.setattr("accel_pipeline.engine_backend.run_host_bootstrap_agent", fake_stage)
    monkeypatch.setattr("accel_pipeline.gates.run_gate", fake_gate)
    monkeypatch.setattr(
        "accel_pipeline.engine_backend.run_critic",
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


def test_host_bootstrap_gate_exhausted_rounds_is_a_clean_stop(tmp_path, monkeypatch):
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

    monkeypatch.setattr("accel_pipeline.engine_backend.load_workload", fake_load)
    monkeypatch.setattr("accel_pipeline.engine_backend.run_host_bootstrap_agent", fake_stage)
    monkeypatch.setattr("accel_pipeline.gates.run_gate", fake_gate)

    with pytest.raises(StageStopped, match="host_bootstrap"):
        run_host_bootstrap(
            pc,
            mc,
            engine_name="postgres",
            representative_workload="tpch_q1_aux",
            force=True,
        )
    assert calls["stage"] == 2  # the first turn, one repair round (host_bootstrap_gate_max_rounds=1)


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

    monkeypatch.setattr("accel_pipeline.engine_backend.load_workload", fake_load)
    monkeypatch.setattr("accel_pipeline.engine_backend.run_host_bootstrap_agent", fake_stage)
    monkeypatch.setattr("accel_pipeline.gates.run_gate", fake_gate)
    monkeypatch.setattr(
        "accel_pipeline.engine_backend.run_critic",
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
    from accel_pipeline.layout import write_allowed

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


def test_recorded_defects_reach_their_owner_once(tmp_path):
    import json

    import accel_pipeline.engine_backend as br

    path = br.requests_path(tmp_path, "anyengine")
    path.parent.mkdir(parents=True, exist_ok=True)
    def request(scope, helper):
        return {"benchmark": "toybench", "query": 3, "scope": scope, "helper": helper, "reason": "found by a run"}

    lines = [
        request("harness", "transaction-safe setup"),
        request("backend", "commit outside the engine lock"),
        {"scope": "backend", "kind": "helper", "reason": "no benchmark, query or helper: names nothing to fix"},
    ]
    path.write_text("".join(json.dumps(line) + "\n" for line in lines))

    assert [r["helper"] for r in br.pending(tmp_path, "anyengine", "harness")] == ["transaction-safe setup"]
    assert [r["helper"] for r in br.pending(tmp_path, "anyengine", "backend")] == ["commit outside the engine lock"]

    br.mark_handled(tmp_path, "anyengine", "backend")
    assert br.pending(tmp_path, "anyengine", "backend") == []
    assert len(br.pending(tmp_path, "anyengine", "harness")) == 1
    br.record(tmp_path, "anyengine", request("backend", "a new one"))
    assert [r["helper"] for r in br.pending(tmp_path, "anyengine", "backend")] == ["a new one"]


def _any_engine() -> str:
    return sorted(p.parent.name for p in (REPO_ROOT / "backends").glob("*/host.sh"))[0]


def test_an_existing_harness_that_passes_the_gate_needs_no_agent_run(tmp_path, monkeypatch):
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    _patch_bootstrap_dirs(tmp_path, monkeypatch)
    calls = {"stage": 0, "gates": 0}

    def fake_stage(*args, **kwargs):
        calls["stage"] += 1

    def fake_gate(*args, **kwargs):
        calls["gates"] += args[1] == "host_bootstrap"
        return GateResult(name=args[1], status="pass", data={"status": "pass"}, path=tmp_path / "hb.json")

    monkeypatch.setattr("accel_pipeline.engine_backend.run_host_bootstrap_agent", fake_stage)
    monkeypatch.setattr("accel_pipeline.gates.run_gate", fake_gate)
    monkeypatch.setattr("accel_pipeline.engine_backend.pending_harness_requests", lambda *a: "")
    out = run_host_bootstrap(pc, load_model_config(repo_root=REPO_ROOT), engine_name=_any_engine(),
                             representative_workload="tpch_q1_aux")
    assert out == {**out, "status": "pass", "skipped": True}
    assert calls == {"stage": 0, "gates": 1}


def _conformance(tmp_path, monkeypatch, statuses):
    """check_conformance with the gate answering `statuses` in turn; returns (bootstrap runs, recorded requests)."""
    import accel_pipeline.engine_backend as hb
    import accel_pipeline.gates as gates_run

    answers = iter(statuses)
    monkeypatch.setattr(gates_run, "run_gate", lambda *a, **k: GateResult(
        "harness_conformance", next(answers), {"reason": "diff_bin exited 1 on query 3", "log_tail": "boom"}))
    boots = []
    monkeypatch.setattr(hb, "run_host_bootstrap", lambda *a, **k: boots.append(k["engine_name"]))
    monkeypatch.setattr(hb, "host_bootstrap_dir", lambda root, engine: tmp_path)
    cfg = SimpleNamespace(repo_root=tmp_path)
    workload = {"workload_kind": "aux_state", "engine_name": "toydb", "benchmark_name": "toybench", "query_number": 3}
    run = lambda: hb.check_conformance(cfg, None, "toybench_q3_aux", workload, 3)  # noqa: E731
    requests = lambda: [json.loads(l) for l in (tmp_path / "pipeline/artifacts/backend_additions/toydb.jsonl").read_text().splitlines()]  # noqa: E731
    return run, boots, requests


def test_harnesses_that_pass_on_the_declining_module_need_no_repair(tmp_path, monkeypatch):
    run, boots, _ = _conformance(tmp_path, monkeypatch, ["pass"])
    run()
    assert boots == []


def test_a_harness_failure_goes_to_host_bootstrap_as_a_request_and_is_checked_again(tmp_path, monkeypatch):
    run, boots, requests = _conformance(tmp_path, monkeypatch, ["fail", "pass"])
    run()
    assert boots == ["toydb"]
    [request] = requests()
    assert request["scope"] == "harness" and request["query"] == 3 and "diff_bin exited 1" in request["reason"]


def test_harnesses_that_keep_failing_stop_the_stage(tmp_path, monkeypatch):
    run, boots, requests = _conformance(tmp_path, monkeypatch, ["fail", "fail", "fail"])
    with pytest.raises(StageStopped, match="harnesses fail on Q3"):
        run()
    assert len(boots) == 2 and len(requests()) == 2 and "boom" in (tmp_path / "stop_report.md").read_text()


@pytest.mark.parametrize("statuses, ran, failed", [
    (["pass", "pass"], ["host_bootstrap", "harness_conformance"], None),
    (["fail"], ["host_bootstrap"], "host_bootstrap"),
    (["pass", "fail"], ["host_bootstrap", "harness_conformance"], "harness_conformance"),
])
def test_the_bootstrap_registers_the_query_then_builds_then_checks_conformance(tmp_path, monkeypatch, statuses, ran, failed):
    import accel_pipeline.engine_backend as hb
    import accel_pipeline.gates as gates_run
    import accel_pipeline.stages as stages

    steps, answers = [], iter(statuses)
    monkeypatch.setattr(stages, "materialize_registration", lambda *a: steps.append("register"))
    monkeypatch.setattr(gates_run, "run_gate", lambda cfg, name, *a, **k: steps.append(name) or GateResult(
        name, next(answers), {"owner": "harness", "reason": "boom"}))
    results, bad = hb.run_bootstrap_gates(SimpleNamespace(repo_root=tmp_path), {"engine_name": "toydb"}, 3, iteration=0)
    assert steps == ["register", *ran] and [r.name for r in results] == ran
    assert (bad.name if bad else None) == failed


def test_only_a_backend_request_a_failing_check_backs_reopens_the_backend(tmp_path, monkeypatch):
    from accel_pipeline import engine_backend as eb

    runs = []
    monkeypatch.setattr(eb, "run_engine_pipeline", lambda *a, **k: runs.append(k["stage"]))
    cfg = SimpleNamespace(repo_root=tmp_path)
    (tmp_path / "pipeline" / "artifacts" / "engines" / "toydb").mkdir(parents=True)
    wish = {"benchmark": "toybench", "query": 1, "scope": "backend", "helper": "an introspection api", "reason": "nice to have"}
    eb.record(tmp_path, "toydb", wish)
    assert not eb.reopen_engine_backend(cfg, None, "toydb") and runs == []
    eb.record(tmp_path, "toydb", {**wish, "check": "aux_exit", "reason": "scan returns stale rows"})
    assert eb.reopen_engine_backend(cfg, None, "toydb") and runs == ["validation"]


def test_host_bootstrap_works_on_the_declining_module_and_puts_the_querys_own_back(tmp_path, monkeypatch):
    from accel_pipeline import engine_backend as eb
    import accel_pipeline.stages as stages

    register = "accelerators/toybench/q3/toydb/aux/register.cpp"
    module = tmp_path / "accelerators/toybench/q3/toydb/aux/module.cpp"
    module.parent.mkdir(parents=True)
    module.write_text("does not compile")
    (module.parent / "register.cpp").write_text("the query's own")
    os.utime(module, (1, 1))  # older than anything a build made during the bootstrap
    monkeypatch.setattr(stages, "materialize_registration",
                        lambda cfg, w, q: (tmp_path / register).parent.mkdir(parents=True) or (tmp_path / register).write_text("declining"))
    workload = {"engine_host": {"aux_register_path": register}, "benchmark_name": "toybench", "engine_name": "toydb"}
    cfg = SimpleNamespace(repo_root=tmp_path)
    with pytest.raises(RuntimeError), eb.declining_query(cfg, workload, 3):
        assert (tmp_path / register).read_text() == "declining" and not module.exists()
        raise RuntimeError("the bootstrap failed")
    assert module.read_text() == "does not compile" and (tmp_path / register).read_text() == "the query's own"
    assert module.stat().st_mtime > time.time() - 60  # newer than the declining module's objects: the build redoes it
