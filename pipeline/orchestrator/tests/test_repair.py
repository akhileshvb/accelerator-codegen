"""The repair loop (triage, retries, quarantine, rollback, replan) and the advisor it consults, and how a failed gate
is told infra from code."""

import json
import subprocess
import threading
from pathlib import Path

import pytest

from accel_pipeline import repair as repair_mod
from accel_pipeline.agent import CodexExecBackend, StageRunResult
from accel_pipeline.config import AdvisorConfig, ModelConfig, PipelineConfig, RepairConfig, SessionConfig
from accel_pipeline.gates import GateResult
from accel_pipeline.repair import Failure, RepairCtx, run_with_repair, StageFailure, StageStopped
from accel_pipeline.agent import MAIN, SessionStore


FAKE_CODEX = Path(__file__).parent / "fixtures" / "fake_codex.sh"
PASS = ([GateResult("aux_correctness", "pass", {})], None)


def gate_fail(name="aux_correctness", text="Segmentation fault in accelerators/toybench/q3/aux.cpp"):
    result = GateResult(name, "fail", {"log_tail": text, "returncode": 1})
    return [result], result


class World:
    """A tmp repo with scripted gates and a recording repair agent."""

    def __init__(self, tmp_path, monkeypatch, **repair):
        self.config = PipelineConfig(
            repo_root=tmp_path, pipeline_root=tmp_path / "pipeline", build_dir=tmp_path / "build",
            codex_home=tmp_path / "home", sandbox="none",
            write_policy="off", repair=RepairConfig(**repair),
        )
        monkeypatch.setattr(repair_mod, "load_run_workload", lambda cfg, name: {"benchmark_name": "toybench"})
        self.gate_script, self.gate_calls, self.agent_messages, self.events = [], [], [], []
        monkeypatch.setattr(repair_mod, "_run_gates", self._run_gates)
        monkeypatch.setattr(repair_mod, "_rollback", lambda ctx: self.events.append("rollback"))
        monkeypatch.setattr(repair_mod, "_replan", lambda ctx, failure: self.events.append("replan") or False)
        monkeypatch.setattr("accel_pipeline.stages.run_agent_stage", self._agent)
        monkeypatch.setattr(repair_mod.time, "sleep", lambda s: None)
        monkeypatch.setattr("accel_pipeline.repair.handle_infra_failure", lambda *a, **k: None)
        self.tmp = tmp_path

    def ctx(self, **kw):
        return RepairCtx(self.config, None, "w", 3, stage="implementation", change_kind="implementation", **kw)

    def _run_gates(self, ctx, names):
        self.gate_calls.append(list(names))
        return self.gate_script.pop(0) if self.gate_script else PASS

    def _agent(self, **kwargs):
        self.agent_messages.append(kwargs["user_message"])


@pytest.fixture
def world(tmp_path, monkeypatch):
    return World(tmp_path, monkeypatch)


def test_passing_gates_return_the_attempt_result(world):
    assert run_with_repair(world.ctx(), lambda: "done", gates=["aux_correctness"]) == "done"
    assert world.agent_messages == []


def test_a_failing_gate_gets_one_repair_round_then_passes(world):
    world.gate_script = [gate_fail()]
    run_with_repair(world.ctx(), lambda: None, gates=["aux_correctness"])
    assert len(world.agent_messages) == 1 and "Segmentation fault" in world.agent_messages[0]
    assert len(world.gate_calls) == 2


def test_exhaustion_rolls_back_replans_then_stops_cleanly(world):
    world.gate_script = [gate_fail()] * 20
    with pytest.raises(StageStopped) as stopped:
        run_with_repair(world.ctx(), lambda: None, gates=["aux_correctness"])
    assert len(world.agent_messages) == 3  # repair.max_code_rounds
    assert world.events == ["rollback", "replan"]
    report = Path(stopped.value.report)
    assert report.name == "stop_report.md" and "Segmentation fault" in report.read_text()
    assert "--resume" in report.read_text()
    log = (world.tmp / "pipeline/artifacts/toybench/q3/experiment_log.md").read_text()
    assert "rolling back" in log and "stopping" in log


def test_the_code_budget_is_configurable(tmp_path, monkeypatch):
    world = World(tmp_path, monkeypatch, max_code_rounds=1)
    world.gate_script = [gate_fail()] * 5
    with pytest.raises(StageStopped):
        run_with_repair(world.ctx(), lambda: None, gates=["aux_correctness"])
    assert len(world.agent_messages) == 1


def test_a_successful_replan_restores_the_code_budget(world, monkeypatch):
    monkeypatch.setattr(repair_mod, "_replan", lambda ctx, failure: world.events.append("replan") or True)
    world.gate_script = [gate_fail()] * 5
    run_with_repair(world.ctx(), lambda: None, gates=["aux_correctness"])
    assert len(world.agent_messages) == 3 and world.events == ["rollback", "replan"]


def test_infra_failures_do_not_spend_the_code_budget(world):
    world.gate_script = [gate_fail(text="run.sh: line 4: ACCEL_X: unbound variable")]
    run_with_repair(world.ctx(), lambda: None, gates=["aux_correctness"])
    assert world.agent_messages == [] and len(world.gate_calls) == 2


def test_a_second_infra_failure_quarantines_the_gate(tmp_path, monkeypatch):
    world = World(tmp_path, monkeypatch, never_quarantine=("aux_unit",))
    infra = "run.sh: line 4: ACCEL_X: unbound variable"
    world.gate_script = [gate_fail("aux_correctness", infra)] * 2
    run_with_repair(world.ctx(), lambda: None, gates=["aux_correctness", "aux_bench_sweep"])
    assert world.gate_calls[-1] == ["aux_bench_sweep"]
    quarantine = json.loads((tmp_path / "pipeline/artifacts/toybench/q3/quarantine.json").read_text())
    assert quarantine[0]["gate"] == "aux_correctness" and quarantine[0]["infra_reason"] == "harness_error"
    assert world.agent_messages == []


def test_a_never_quarantine_gate_stops_the_run_instead(world):
    infra = "run.sh: line 4: ACCEL_X: unbound variable"
    world.gate_script = [gate_fail("aux_correctness", infra)] * 2
    with pytest.raises(StageStopped, match="infra failure persists"):
        run_with_repair(world.ctx(), lambda: None, gates=["aux_correctness"])
    assert world.agent_messages == [] and world.events == []


def test_a_baseline_measurement_failure_resumes_the_main_session_and_reruns_the_attempt(world):
    calls = []

    def attempt():
        calls.append(1)
        if len(calls) == 1:
            raise StageFailure(Failure(kind="measurement", stage="optimization", source="bench_baseline",
                                       summary="the baseline benchmark did not produce a measurement"))
        return "ok"

    assert run_with_repair(world.ctx(), attempt) == "ok"
    assert len(world.agent_messages) == 1
    assert "measurement failure" in world.agent_messages[0] and "bench_baseline" in world.agent_messages[0]


def test_a_codex_timeout_resumes_the_agent_and_rechecks_by_gate(world):
    timeout = Failure(kind="timeout", stage="implementation", source="agent", summary="codex exec idle for 2400s",
                      diagnosis=["You were stopped at the deadline. State your status, checkpoint"])
    attempts = []

    def attempt():
        attempts.append(1)
        raise StageFailure(timeout)

    run_with_repair(world.ctx(), attempt, gates=["aux_correctness"])
    assert attempts == [1]  # the agent continues; the gates judge, the attempt is not re-run
    assert "stopped at the deadline" in world.agent_messages[0]


def test_an_unknown_exception_is_retried_once_then_stops(world):
    def attempt():
        raise ValueError("bug")

    with pytest.raises(StageStopped, match="orchestrator failure"):
        run_with_repair(world.ctx(), attempt)
    assert world.agent_messages == [] and world.events == []
    assert "Traceback" in (world.tmp / "pipeline/artifacts/toybench/q3/stop_report.md").read_text()


def test_a_provider_flake_reruns_the_attempt_not_the_gates(world):
    flake = Failure(kind="infra", stage="implementation", source="agent", summary="x", infra_reason="provider_flake")
    attempts = []

    def attempt():
        attempts.append(1)
        if len(attempts) == 1:
            raise StageFailure(flake)
        return "ok"

    assert run_with_repair(world.ctx(), attempt, gates=["aux_correctness"]) == "ok"
    assert len(attempts) == 2 and len(world.gate_calls) == 1


FLAKE = Failure(kind="infra", stage="implementation", source="agent", summary="x", infra_reason="provider_flake")


def test_provider_flakes_have_their_own_budget_and_back_off(world, monkeypatch):
    waits = []
    monkeypatch.setattr(repair_mod.time, "sleep", waits.append)
    attempts = []

    def attempt():
        attempts.append(1)
        if len(attempts) <= 3:
            raise StageFailure(FLAKE)
        return "ok"

    assert run_with_repair(world.ctx(), attempt) == "ok"
    assert waits == [30.0, 90.0, 270.0] and world.agent_messages == []


def test_a_flake_that_never_clears_stops_the_run_cleanly(world):
    def attempt():
        raise StageFailure(FLAKE)

    with pytest.raises(StageStopped, match="provider failed 4 times"):
        run_with_repair(world.ctx(), attempt)


def test_flakes_do_not_spend_the_infra_or_code_budgets(tmp_path, monkeypatch):
    world = World(tmp_path, monkeypatch, infra_retries=0, max_code_rounds=0, provider_retries=2)
    attempts = []

    def attempt():
        attempts.append(1)
        if len(attempts) <= 2:
            raise StageFailure(FLAKE)

    run_with_repair(world.ctx(), attempt)
    assert len(attempts) == 3


def test_a_flake_in_the_repair_turn_reruns_the_turn_not_the_code_round(world, monkeypatch):
    turns = []

    def agent(**kwargs):
        turns.append(1)
        if len(turns) == 1:
            raise StageFailure(FLAKE)
        world.agent_messages.append(kwargs["user_message"])

    monkeypatch.setattr("accel_pipeline.stages.run_agent_stage", agent)
    world.gate_script = [gate_fail()]
    run_with_repair(world.ctx(), lambda: None, gates=["aux_correctness"])
    assert len(turns) == 2 and len(world.agent_messages) == 1 and len(world.gate_calls) == 2


def test_a_nested_stop_passes_through(world):
    def attempt():
        raise StageStopped("inner", Path("x"))

    with pytest.raises(StageStopped, match="inner"):
        run_with_repair(world.ctx(), attempt)


def test_a_failing_repair_turn_does_not_escape(world, monkeypatch):
    def broken(**kwargs):
        raise RuntimeError("codex crashed")

    monkeypatch.setattr("accel_pipeline.stages.run_agent_stage", broken)
    world.gate_script = [gate_fail()]
    run_with_repair(world.ctx(), lambda: None, gates=["aux_correctness"])


def test_a_crashing_gate_runner_is_an_orchestrator_failure(world, monkeypatch):
    def boom(ctx, names):
        raise FileNotFoundError("gate script")

    monkeypatch.setattr(repair_mod, "_run_gates", boom)
    with pytest.raises(StageStopped):
        run_with_repair(world.ctx(), lambda: None, gates=["aux_correctness"])


def _stage_world(tmp_path, monkeypatch, mode="ok"):
    """Real run_agent_stage and CodexExecBackend on a fake codex; only prompts, logs and gates are stubbed."""
    import accel_pipeline.stages as base

    world = World(tmp_path, monkeypatch)
    monkeypatch.undo()  # World's stubs are for the unit tests; re-stub what this one needs
    monkeypatch.setattr(repair_mod, "load_run_workload", lambda cfg, name: {"benchmark_name": "toybench"})
    monkeypatch.setattr(repair_mod, "_run_gates", world._run_gates)
    monkeypatch.setattr(repair_mod, "_rollback", lambda ctx: None)
    monkeypatch.setattr(repair_mod, "_replan", lambda ctx, failure: False)
    monkeypatch.setattr("accel_pipeline.repair.handle_infra_failure", lambda *a, **k: None)
    monkeypatch.setenv("FAKE_CODEX_MODE", mode)
    monkeypatch.setenv("FAKE_CODEX_DIR", str(tmp_path))
    (tmp_path / "accelerators/toybench/q3").mkdir(parents=True)
    monkeypatch.setenv("FAKE_CODEX_TOUCH", str(tmp_path / "accelerators/toybench/q3/aux.cpp"))
    models_cfg = ModelConfig(api_key_env="K", presets={}, stages={}, codex_bin=str(FAKE_CODEX))
    backend = CodexExecBackend(pipeline_config=world.config, model_config=models_cfg, query=3, stage="x",
                               art_dir=tmp_path / "art")
    for name, value in {
        "load_run_workload": lambda cfg, name: {"benchmark_name": "toybench"},
        "apply_workload_overrides": lambda cfg, workload: cfg,
        "render_prompt": lambda *a, **k: "INSTRUCTIONS",
        "save_rendered_prompt": lambda *a, **k: None,
        "with_codex_tooling": lambda cfg, prompt, **_: prompt,
        "make_model_id": lambda *a, **k: "m",
        "create_agent_backend": lambda **k: backend,
        "log_stage_start": lambda *a, **k: None,
        "log_stage_complete": lambda *a, **k: None,
        "frozen_snapshot": lambda root: {},
        "frozen_restore": lambda root, snap: [],
    }.items():
        monkeypatch.setattr(base, name, value)
    monkeypatch.setattr(base, "resolve_stage_provider", lambda stage, cfg: "codex_exec")
    monkeypatch.setattr("accel_pipeline.stages.refresh_for_stage", lambda root: None)
    world.models, world.backend = models_cfg, backend
    return world


def _implement(world):
    from accel_pipeline.agent import MAIN
    from accel_pipeline.stages import run_agent_stage

    return lambda: run_agent_stage(
        stage="implementation", prompt_file="02_implementation.md", pipeline_config=world.config,
        model_config=world.models, workload_name="w", query=3, user_message="implement", session_role=MAIN)


def test_a_failing_gate_is_repaired_over_two_rounds_in_one_session(tmp_path, monkeypatch):
    world = _stage_world(tmp_path, monkeypatch)
    world.gate_script = [gate_fail(), gate_fail()]
    ctx = RepairCtx(world.config, world.models, "w", 3, stage="implementation", change_kind="implementation")
    run_with_repair(ctx, _implement(world), gates=["aux_correctness"])

    args = (tmp_path / "args.log").read_text().splitlines()
    assert len(args) == 3  # implementation + two repair rounds
    assert "exec resume" not in args[0] and all("exec resume" in a for a in args[1:])
    sid = "11111111-2222-3333-4444-555555555555"
    assert all(sid in a for a in args[1:])
    assert json.loads((tmp_path / "pipeline/artifacts/toybench/q3/sessions.json").read_text()) == {"3": {"main": sid}}
    prompts = (tmp_path / "prompts.log").read_text()
    assert prompts.count("INSTRUCTIONS") == 2  # implementation once, then the repair phase's once
    assert prompts.count("## code failure in implementation") == 2


def test_exhausted_repair_stops_cleanly_without_an_exception_from_the_stage(tmp_path, monkeypatch):
    world = _stage_world(tmp_path, monkeypatch)
    world.gate_script = [gate_fail()] * 10
    ctx = RepairCtx(world.config, world.models, "w", 3, stage="implementation", change_kind="implementation")
    with pytest.raises(StageStopped):
        run_with_repair(ctx, _implement(world), gates=["aux_correctness"])
    assert len((tmp_path / "args.log").read_text().splitlines()) == 4  # implementation + 3 rounds
    assert (tmp_path / "pipeline/artifacts/toybench/q3/stop_report.md").is_file()


def test_a_killed_agent_is_resumed_not_restarted(tmp_path, monkeypatch):
    world = _stage_world(tmp_path, monkeypatch, mode="sleep")
    world.config.session = SessionConfig(idle_timeout_sec=1, hard_deadline_sec=60)
    def attempt():
        monkeypatch.setenv("FAKE_CODEX_MODE", "sleep")
        try:
            return _implement(world)()
        finally:
            monkeypatch.setenv("FAKE_CODEX_MODE", "ok")

    run_with_repair(RepairCtx(world.config, world.models, "w", 3, stage="implementation",
                              change_kind="implementation"), attempt, gates=["aux_correctness"])
    args = (tmp_path / "args.log").read_text().splitlines()
    assert "exec resume" not in args[0] and "exec resume" in args[1]
    assert "stopped at the deadline" in (tmp_path / "prompts.log").read_text()


def test_quick_first_fails_the_batch_before_the_heavy_gates(tmp_path, monkeypatch):
    from accel_pipeline.gates import QuickResult

    world = World(tmp_path, monkeypatch)
    monkeypatch.undo()  # the real _run_gates, not World's scripted one
    monkeypatch.setattr(repair_mod, "load_run_workload", lambda cfg, name: {"benchmark_name": "toybench"})
    heavy = []
    monkeypatch.setattr("accel_pipeline.gates.run_gates_parallel", lambda cfg, names, *a, **k: heavy.append(names) or [])
    monkeypatch.setattr("accel_pipeline.stages.run_post_stage_gates_collect", lambda cfg, w, q, names, **k: heavy.append(names) or ([], None))
    verdicts = [QuickResult(status="fail", summary="quick_check: projected full-gate write time ~900 s (budget 450 s)"),
                QuickResult(status="pass")]
    monkeypatch.setattr("accel_pipeline.gates.run_quick_check", lambda *a, **k: verdicts.pop(0))
    ctx = world.ctx(quick_first=True)
    results, failed = repair_mod._run_gates(ctx, ["aux_correctness"])
    assert failed.name == "quick_check" and "projected" in failed.data["reason"] and heavy == []
    repair_mod._run_gates(ctx, ["aux_correctness"])
    assert heavy == [["aux_correctness"]]


def _blocked():
    from accel_pipeline.repair import AGENT_BLOCKER, Failure, StageFailure

    raise StageFailure(Failure(kind="infra", stage="implementation", source="agent", summary="the gate looks stuck",
                               infra_reason=AGENT_BLOCKER))


def test_a_blocker_from_the_stage_turn_runs_the_gates_once_and_passes_on_them(world):
    run_with_repair(world.ctx(), _blocked, gates=["aux_correctness"])
    assert world.gate_calls == [["aux_correctness"]] and world.agent_messages == []


def test_a_blocker_the_gates_confirm_gets_a_round_to_adapt(world):
    world.gate_script = [gate_fail()]
    run_with_repair(world.ctx(), _blocked, gates=["aux_correctness"])
    assert len(world.agent_messages) == 1 and len(world.gate_calls) == 2
    assert "the gate looks stuck" in world.agent_messages[0] and repair_mod.ADAPT in world.agent_messages[0]


def test_a_blocker_without_gates_gets_a_round_to_adapt(world):
    attempts = [_blocked, lambda: "done"]
    assert run_with_repair(world.ctx(), lambda: attempts.pop(0)()) == "done"
    assert len(world.agent_messages) == 1 and repair_mod.ADAPT in world.agent_messages[0]


def test_a_blocker_stops_the_stage_only_once_the_repair_budget_is_spent(tmp_path, monkeypatch):
    world = World(tmp_path, monkeypatch, max_code_rounds=2)
    world.gate_script = [gate_fail()] * 4  # the first check, one per round, one after the rollback
    with pytest.raises(StageStopped) as stopped:
        run_with_repair(world.ctx(), _blocked, gates=["aux_correctness"])
    assert len(world.agent_messages) == 2 and world.events == ["rollback", "replan"]
    report = Path(stopped.value.report).read_text()
    assert "the gate looks stuck" in report and "survived 2 repair rounds" in report


def test_a_blocker_on_the_frozen_suite_goes_to_the_tester_then_the_gates_rerun(world, monkeypatch):
    revisions = [True, False]
    monkeypatch.setattr(repair_mod, "revise_pending_suite", lambda ctx: revisions.pop(0))
    run_with_repair(world.ctx(), _blocked, gates=["aux_correctness"])
    assert world.gate_calls == [["aux_correctness"]] and revisions == [False] and world.agent_messages == []


def _out_of_credits():
    from accel_pipeline.repair import Failure, StageFailure
    from accel_pipeline.stages import stage_failure

    result = type("R", (), {"error": "unexpected status 402 Payment Required: This request requires more credits",
                            "status": "error", "metadata": {}, "final_text": ""})()
    raise StageFailure(stage_failure("implementation", "codex_exec", result))


def test_an_out_of_credits_provider_stops_at_once_without_retries(world):
    with pytest.raises(StageStopped) as stopped:
        run_with_repair(world.ctx(), _out_of_credits, gates=["aux_correctness"])
    assert "out of credits" in stopped.value.reason and world.agent_messages == [] and world.gate_calls == []


SID = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"


def git(root, *args):
    subprocess.run(["git", "-C", str(root), "-c", "user.email=a@b", "-c", "user.name=t", *args], check=True,
                   capture_output=True)


class FakeFork:
    """Stands in for the advisor's backend: records the request and edits files like a misbehaving repair_mod."""

    def __init__(self, root, advice="Root cause: X.\nNext: Y.", status="finished"):
        self.root, self.advice, self.status, self.requests = root, advice, status, []

    def fork_stage(self, request):
        self.requests.append(request)
        (self.root / "accelerators/q3.cpp").write_text("advisor edit")  # tracked, modified
        (self.root / "accelerators/new.cpp").write_text("advisor file")  # untracked
        (self.root / "accelerators/dirty.cpp").write_text("advisor rewrote the agent's work")
        return StageRunResult(backend="codex_exec", stage=request.stage, status=self.status, final_text=self.advice)


class AdvisorWorld:
    def __init__(self, tmp_path, monkeypatch, **advisor_cfg):
        self.root = tmp_path
        (tmp_path / "accelerators").mkdir()
        (tmp_path / "accelerators/q3.cpp").write_text("committed")
        (tmp_path / "accelerators/dirty.cpp").write_text("committed")
        git(tmp_path, "init")
        git(tmp_path, "add", "-A")
        git(tmp_path, "commit", "-m", "init")
        (tmp_path / "accelerators/dirty.cpp").write_text("the agent's work in progress")
        self.config = PipelineConfig(
            repo_root=tmp_path, pipeline_root=tmp_path / "pipeline", build_dir=tmp_path / "build",
            codex_home=tmp_path / "home", sandbox="none",
            write_policy="off", repair=RepairConfig(), advisor=AdvisorConfig(**advisor_cfg),
        )
        self.models = ModelConfig(api_key_env="K", presets={}, stages={}, codex_bin=str(FAKE_CODEX))
        self.art = tmp_path / "pipeline/artifacts/toybench/q3"
        self.art.mkdir(parents=True)
        self.fork = FakeFork(tmp_path)
        workload = {"benchmark_name": "toybench"}
        monkeypatch.setattr(repair_mod, "load_run_workload", lambda cfg, name: workload)
        monkeypatch.setattr(repair_mod, "make_model_id", lambda *a, **k: "advisor/model")
        monkeypatch.setattr(repair_mod, "render_prompt", lambda *a, **k: "ADVISOR INSTRUCTIONS")
        monkeypatch.setattr("accel_pipeline.agent.create_agent_backend", lambda **k: self.backend(**k))
        self.created = []

    def backend(self, **kwargs):
        self.created.append(kwargs)
        return self.fork

    def consult(self, stage="implementation", question="why?", trigger="agent", **kw):
        return repair_mod.consult(self.config, self.models, "w", 3, stage, question=question, trigger=trigger, **kw)

    def main_session(self, sid=SID):
        SessionStore(self.art).record(3, MAIN, sid, "implementation", resumed=False)

    def events(self):
        path = next((self.root / "pipeline/artifacts").rglob("pipeline_events.jsonl"))
        return [json.loads(line) for line in path.read_text().splitlines()]


@pytest.fixture
def advisor_world(tmp_path, monkeypatch):
    return AdvisorWorld(tmp_path, monkeypatch)


# ---- the fork command ----

def fork_backend(tmp_path, monkeypatch, art=True):
    monkeypatch.setenv("FAKE_CODEX_MODE", "ok")
    monkeypatch.setenv("FAKE_CODEX_DIR", str(tmp_path))
    config = PipelineConfig(repo_root=tmp_path, pipeline_root=tmp_path / "pipeline", build_dir=tmp_path / "build",
                            codex_home=tmp_path / "home",
                            sandbox="none", write_policy="enforce")
    models = ModelConfig(api_key_env="K", presets={}, stages={}, codex_bin=str(FAKE_CODEX))
    return CodexExecBackend(pipeline_config=config, model_config=models, query=3, stage="implementation",
                            art_dir=tmp_path / "art" if art else None)


def test_the_fork_command_forks_the_session_on_the_advisor_model(tmp_path, monkeypatch):
    from accel_pipeline.agent import StageRunRequest

    request = StageRunRequest(stage="advisor", instructions="INSTR", user_message="why?", agent_name="a",
                              model_id="adv/model", agent_model="adv/model", metadata={"fork_session_id": SID})
    result = fork_backend(tmp_path, monkeypatch).fork_stage(request)
    assert result.status == "finished"
    args = (tmp_path / "args.log").read_text().splitlines()
    assert len(args) == 1
    assert args[0].startswith("exec fork --skip-git-repo-check -m adv/model ")
    assert "resume" not in args[0] and " -C " not in args[0]
    assert args[0].endswith(f"{SID} -")  # the prompt comes on stdin
    assert 'enabled_tools=["quick_check"' in args[0] and "request_gate" not in args[0]
    assert "ask_advisor" not in args[0]
    assert not (tmp_path / "art").exists()  # no run file, progress clock or heartbeat: the agent's are untouched


def test_a_fork_never_serves_advisor_requests_or_marks_the_agent_run(tmp_path, monkeypatch):
    backend = fork_backend(tmp_path, monkeypatch)
    from accel_pipeline.agent import StageRunRequest

    (tmp_path / "art").mkdir()
    (tmp_path / "art" / "agent_run.json").write_text("{}")
    backend.fork_stage(StageRunRequest(stage="advisor", instructions="I", user_message="q", agent_name="a",
                                       model_id="m", metadata={"fork_session_id": SID}))
    assert (tmp_path / "art" / "agent_run.json").exists()


# ---- consult ----

def test_consult_forks_the_main_session_and_undoes_the_advisors_edits(advisor_world):
    advisor_world.main_session()
    advice = advisor_world.consult()
    assert advice.startswith("Root cause: X.")
    (request,) = advisor_world.fork.requests
    assert request.metadata == {"fork_session_id": SID} and request.stage == "advisor"
    assert request.user_message == "why?" and request.instructions == "ADVISOR INSTRUCTIONS"
    assert advisor_world.created[0]["stage"] == "implementation"  # the agent's MCP setup
    root = advisor_world.root / "accelerators"
    assert (root / "q3.cpp").read_text() == "committed"
    assert not (root / "new.cpp").exists()
    assert (root / "dirty.cpp").read_text() == "the agent's work in progress"


def test_a_live_session_id_serves_a_first_turn_without_a_stored_one(advisor_world):
    advisor_world.consult(session_id="live-id")
    assert advisor_world.fork.requests[0].metadata["fork_session_id"] == "live-id"


def test_no_session_no_consult_and_no_count(advisor_world):
    with pytest.raises(repair_mod.Unavailable, match="no session"):
        advisor_world.consult()
    assert repair_mod.consults_used(advisor_world.art, "implementation") == 0 and not advisor_world.fork.requests


def test_consults_are_capped_per_stage_family(tmp_path, monkeypatch):
    advisor_world = AdvisorWorld(tmp_path, monkeypatch, max_consults_per_stage=2)
    advisor_world.main_session()
    advisor_world.consult()
    advisor_world.consult(stage="impl_host")  # same family
    with pytest.raises(repair_mod.Unavailable, match="limit reached"):
        advisor_world.consult()
    advisor_world.consult(stage="optimization_unified")  # its own cap
    assert len(advisor_world.fork.requests) == 3


def test_a_disabled_advisor_is_unavailable(tmp_path, monkeypatch):
    advisor_world = AdvisorWorld(tmp_path, monkeypatch, max_consults_per_stage=0)
    advisor_world.main_session()
    with pytest.raises(repair_mod.Unavailable, match="disabled"):
        advisor_world.consult()


def test_a_consult_is_logged_as_an_event_and_in_the_experiment_log(advisor_world):
    advisor_world.main_session()
    advisor_world.consult(trigger="stall")
    (event,) = [e for e in advisor_world.events() if e["event"] == "advisor_consult"]
    assert event["trigger"] == "stall" and event["stage"] == "implementation" and event["consult"] == 1
    assert event["session_id"] == SID and event["model"] == "advisor/model" and event["edits_undone"] == 3
    log = (advisor_world.art / "experiment_log.md").read_text()
    assert "advisor[implementation] consult 1/4 (stall" in log


def test_an_advisor_that_fails_gives_no_advice(advisor_world):
    advisor_world.main_session()
    advisor_world.fork.status, advisor_world.fork.advice = "error", ""
    with pytest.raises(repair_mod.Unavailable, match="no answer"):
        advisor_world.consult()
    assert (advisor_world.root / "accelerators/dirty.cpp").read_text() == "the agent's work in progress"


# ---- the agent's tool ----

def serve(advisor_world, stage="implementation"):
    return repair_mod.Servicer(advisor_world.config, advisor_world.models, "w", 3, stage, 0, advisor_world.art)


def test_ask_gets_the_servicers_answer(advisor_world):
    advisor_world.main_session()
    servicer = serve(advisor_world)
    answers = []
    asker = threading.Thread(target=lambda: answers.append(repair_mod.ask(advisor_world.art, "stuck on X", 20, poll_sec=0.05)))
    asker.start()
    while asker.is_alive():
        servicer.poll()
        asker.join(0.05)
    servicer.join()
    assert answers[0].startswith("Root cause: X.")
    assert advisor_world.fork.requests[0].user_message == "stuck on X"
    assert not (advisor_world.art / repair_mod.REQUEST_FILE).exists()


def test_ask_reports_a_refusal(advisor_world):
    servicer = serve(advisor_world)  # no session yet
    asker = threading.Thread(target=lambda: advisor_world.__dict__.update(answer=repair_mod.ask(advisor_world.art, "q", 20, poll_sec=0.05)))
    asker.start()
    while asker.is_alive():
        servicer.poll()
        asker.join(0.05)
    assert advisor_world.answer.startswith("No advice: this stage has no session")


def test_ask_gives_up_when_nobody_serves(advisor_world):
    assert "did not answer in time" in repair_mod.ask(advisor_world.art, "q", 0.2, poll_sec=0.05)
    assert not (advisor_world.art / repair_mod.REQUEST_FILE).exists()


def mcp_tools(advisor_world, stage="implementation"):
    from accel_pipeline.mcp import build_mcp_tool_callables

    return build_mcp_tool_callables(advisor_world.config, {"benchmark_name": "toybench"}, "w", 3, stage, 0, advisor_world.art)


def test_the_mcp_tool_counts_down_and_stops_at_the_cap(tmp_path, monkeypatch):
    advisor_world = AdvisorWorld(tmp_path, monkeypatch, max_consults_per_stage=1)
    monkeypatch.setattr(repair_mod, "ask", lambda art, question, timeout: "advice")
    ask = mcp_tools(advisor_world)["ask_advisor"]
    assert ask("q").endswith("[advisor consults left in this stage: 0]")
    advisor_world.main_session()
    advisor_world.consult()  # the servicer's side spends the one consult
    assert "limit reached" in ask("q")


def test_the_mcp_tool_is_for_implementation_optimization_and_their_repair_turns(advisor_world, monkeypatch):
    monkeypatch.setattr(repair_mod, "ask", lambda art, question, timeout: "advice")
    assert "not available" in mcp_tools(advisor_world, stage="analysis")["ask_advisor"]("q")
    assert mcp_tools(advisor_world, stage="crash_debug")["ask_advisor"]("q").startswith("advice")


# ---- the repair loop ----

class Loop:
    def __init__(self, advisor_world, monkeypatch, texts):
        self.advisor_world, self.messages, self.consults = advisor_world, [], []
        self.texts = list(texts)
        monkeypatch.setattr(repair_mod, "load_run_workload", lambda cfg, name: {"benchmark_name": "toybench"})
        monkeypatch.setattr(repair_mod, "_run_gates", self._gates)
        monkeypatch.setattr(repair_mod, "_rollback", lambda ctx: None)
        monkeypatch.setattr(repair_mod, "_replan", lambda ctx, failure: False)
        monkeypatch.setattr(repair_mod, "consult", self._consult)

    def _gates(self, ctx, names):
        from accel_pipeline.gates import GateResult

        if not self.texts:
            return [GateResult("g", "pass", {})], None
        result = GateResult("g", "fail", {"log_tail": self.texts.pop(0), "returncode": 1})
        return [result], result

    def _consult(self, *args, **kw):
        self.consults.append(kw)
        return "use the delta path"

    def run(self, **kw):
        ctx = RepairCtx(self.advisor_world.config, None, "w", 3, stage="implementation", change_kind="implementation",
                        repair_turn=lambda failure: self.messages.append(repair_mod.render_failure(failure)), **kw)
        run_with_repair(ctx, lambda: None, gates=["g"])


def test_the_same_failure_twice_brings_the_advisor_into_the_next_message(advisor_world, monkeypatch):
    loop = Loop(advisor_world, monkeypatch, ["quick_check failed: rebuild on write", "quick_check failed: rebuild on write"])
    loop.run()
    assert len(loop.consults) == 1 and loop.consults[0]["trigger"] == "stall"
    assert "A reviewer with your full context diagnosed:\nuse the delta path" not in loop.messages[0]
    assert "A reviewer with your full context diagnosed:\nuse the delta path" in loop.messages[1]


def test_a_changing_failure_is_not_a_stall(advisor_world, monkeypatch):
    loop = Loop(advisor_world, monkeypatch, ["error one", "error two", "error three"])
    loop.run()
    assert loop.consults == []


def test_digits_do_not_make_a_failure_new(advisor_world, monkeypatch):
    loop = Loop(advisor_world, monkeypatch, ["failed at 12.5s, 3 of 9", "failed at 13.1s, 4 of 9"])
    loop.run()
    assert len(loop.consults) == 1


def test_one_consult_per_stall(advisor_world, monkeypatch):
    loop = Loop(advisor_world, monkeypatch, ["same"] * 3)
    loop.run()
    assert len(loop.consults) == 1


def test_a_no_progress_redirect_consults_at_once(advisor_world, monkeypatch):
    advisor_world.config.repair = RepairConfig()
    loop = Loop(advisor_world, monkeypatch, [])
    failure = Failure(kind="timeout", stage="implementation", source="no_progress", summary="no progress for 1200s")
    attempts = iter([failure])

    def attempt():
        from accel_pipeline.repair import StageFailure

        for f in attempts:
            raise StageFailure(f)

    ctx = RepairCtx(advisor_world.config, None, "w", 3, stage="implementation", change_kind="implementation",
                    repair_turn=lambda f: loop.messages.append(repair_mod.render_failure(f)))
    run_with_repair(ctx, attempt, gates=["g"])
    assert len(loop.consults) == 1 and "A reviewer with your full context" in loop.messages[0]


def test_an_unavailable_advisor_does_not_stop_the_repair(advisor_world, monkeypatch):
    loop = Loop(advisor_world, monkeypatch, ["same", "same"])

    def refuse(*a, **kw):
        raise repair_mod.Unavailable("advisor limit reached (4 consults in implementation)")

    monkeypatch.setattr(repair_mod, "consult", refuse)
    loop.run()
    assert len(loop.messages) == 2 and all("A reviewer" not in m for m in loop.messages)


# ---- configuration ----


WORKLOAD = {"benchmark_name": "toybench", "query_number": 3}

TSAN = """WARNING: ThreadSanitizer: data race (pid=1)
  Write of size 8 at 0x1 by thread T1:
    #0 do_work /engine/src/pool.cc:88 (libengine.so+0x1)
    #1 run /engine/src/pool.cc:90 (libengine.so+0x2)
SUMMARY: ThreadSanitizer: data race /engine/src/pool.cc:88 in do_work
"""
TSAN_OURS = TSAN.replace("/engine/src/pool.cc:88 (libengine", "/repo/accelerators/toybench/q3/aux.cpp:88 (libengine", 1)


@pytest.fixture
def repo(tmp_path):
    subprocess.run(["git", "init", "-q"], cwd=tmp_path, check=True)
    subprocess.run(["git", "-c", "user.email=a@b", "-c", "user.name=n", "commit", "-q", "--allow-empty", "-m", "i"],
                   cwd=tmp_path, check=True)
    return tmp_path


def classify(text, repo, **kw):
    return repair_mod.classify("aux_correctness", {"log_tail": text, **kw}, WORKLOAD, repo)


def test_engine_internal_sanitizer_is_infra(repo):
    assert classify(TSAN, repo) == ("infra", "engine_internal_sanitizer")


def test_sanitizer_frame_in_the_query_dir_is_code(repo):
    assert classify(TSAN_OURS, repo) == ("code", None)


def test_sanitizer_frame_in_a_file_changed_this_stage_is_code(repo):
    (repo / "shared.cc").write_text("x")
    text = TSAN.replace("/engine/src/pool.cc:88 (libengine", "/repo/shared.cc:88 (libengine", 1)
    assert classify(text, repo) == ("code", None)


def test_missing_producer_artifact(repo):
    assert classify("error: analysis.json not found for query 3", repo) == ("infra", "missing_producer")
    assert classify("target missing from the gate context", repo) == ("infra", "missing_producer")


@pytest.mark.parametrize("text", [
    "run.sh: line 4: ACCEL_X: unbound variable",
    "Traceback (most recent call last):\n  File \"/r/pipeline/gates/read_gate_context.py\", line 3, in <module>\nKeyError: 'a'",
    "backends/e/host.sh up exited with status 2",
])
def test_harness_errors(text, repo):
    assert classify(text, repo) == ("infra", "harness_error")


def test_python_traceback_outside_the_harness_is_code(repo):
    text = "Traceback (most recent call last):\n  File \"/r/accelerators/toybench/q3/check.py\", line 3\nValueError"
    assert classify(text, repo) == ("code", None)


def test_timeout_is_infra_only_under_load(repo, monkeypatch):
    text = "gate did not finish within 7200s"
    monkeypatch.setattr("os.getloadavg", lambda: (0.1, 0.1, 0.1))
    assert classify(text, repo) == ("code", None)
    monkeypatch.setattr("os.getloadavg", lambda: (1e6, 1e6, 1e6))
    assert classify(text, repo) == ("infra", "timeout_under_load")
    assert classify("", repo, returncode=137) == ("infra", "timeout_under_load")


def test_shared_tree_failures_are_infra(repo):
    assert classify("unsupported shared query binding", repo) == ("infra", "unsupported_shared_query")


def test_plain_test_failure_is_code(repo):
    assert classify("Segmentation fault in accelerators/toybench/q3/aux.cpp\n1 test failed", repo) == ("code", None)


def test_transient_provider_failure():
    assert repair_mod.is_transient_provider_failure("stream disconnected before completion")
    assert not repair_mod.is_transient_provider_failure("assertion failed in kernel")
    assert not repair_mod.is_transient_provider_failure(None)


ENGINE_WORKLOAD = {"workload_kind": "engine_backend", "engine_name": "toydb"}
ASAN_BACKEND = """==1==ERROR: AddressSanitizer: stack-use-after-return on address 0x1
READ of size 3 at 0x1 thread T0
    #0 0x1 in toydb::TextAccessor::TextView /repo/backends/toydb/api/scan.hpp:99
    #1 0x2 in main /repo/pipeline/canary/canary_mirror.hpp:7
"""
ASAN_ENGINE = """==1==ERROR: AddressSanitizer: heap-use-after-free on address 0x1
    #0 0x1 in engine::Pool::Run /engine/src/pool.cc:88
    #1 0x2 in engine::Pool::Loop /engine/src/pool.cc:90
"""


def test_an_engine_backend_report_in_the_backends_tree_is_code(repo):
    assert repair_mod.classify("engine_canary_asan", {"log_tail": ASAN_BACKEND}, ENGINE_WORKLOAD, repo) == ("code", None)


def test_an_engine_backend_report_without_a_backend_frame_is_infra(repo):
    assert repair_mod.classify("engine_canary_asan", {"log_tail": ASAN_ENGINE}, ENGINE_WORKLOAD, repo) == (
        "infra", "engine_internal_sanitizer")


def test_a_harness_that_fails_a_later_repeat_is_a_race_and_code():
    data = {"log_tail": "test_x failed on run 4 after passing 3 times: a race (the harness or the backend depends on timing)"}
    assert repair_mod.racy_repeats(data) == (3, 4)
    assert repair_mod.racy_repeats({"log_tail": "failed on run 1"}) is None
    assert repair_mod.classify("engine_scan", data, ENGINE_WORKLOAD, None) == ("code", None)


def test_the_failure_report_of_a_racy_gate_says_it_is_a_race():
    from accel_pipeline.repair import failure_from_gate
    from accel_pipeline.gates import GateResult

    result = GateResult("engine_scan", "fail", {"reason": "t failed on run 4 after passing 3 times: a race"})
    failure = failure_from_gate(result, stage="impl_scan", workload=ENGINE_WORKLOAD)
    assert failure.kind == "code" and "passed 3 of 4 repeats" in failure.diagnosis[0]


def test_registration_errors_in_suite_validation_are_infra(repo):
    from accel_pipeline.repair import classify as gate_classify

    text = "aux warmup failed for Q1: Catalog Error: Table Function with name accel_x_q1_aux does not exist!"
    assert gate_classify("test_suite_validate", {"log_tail": text}, {}, repo)[0] == "infra"
    assert gate_classify("aux_correctness", {"log_tail": text}, {}, repo)[0] == "code"


HOSTED = {"benchmark_name": "toybench", "query_number": 3, "engine_name": "toydb"}


def test_a_missing_harness_target_is_host_bootstraps_not_the_querys(repo):
    text = "[host.sh build test_toydb_cross_accel] rc=1\nninja: error: unknown target 'test_toydb_cross_accel'"
    assert repair_mod.classify("aux_exit", {"log_tail": text}, HOSTED, repo) == ("infra", repair_mod.HARNESS_MISSING)
    assert repair_mod.classify("host_bootstrap", {"log_tail": text}, HOSTED, repo)[1] != repair_mod.HARNESS_MISSING
    own = "ninja: error: unknown target 'agent_toybench_q3_oracle'"  # the query's own target is the query's to fix
    assert repair_mod.classify("quick_check", {"log_tail": own}, HOSTED, repo) == ("code", None)


def test_the_failure_report_carries_the_reason_the_stacks_one_log_tail_and_the_gate_to_rerun():
    from accel_pipeline.gates import GateResult

    data = {"reason": "cross_accel: test_toydb_cross_accel did not finish within 900s", "exit_code": 124,
            "stacks": "== process 7 (harness) ==\n  3 threads:\n    Pool::Wait  pool.cpp:88",
            "log_tail": "workload=toybench query=3 argument_seed=1\nworkload=toybench query=3 argument_seed=2"}
    f = repair_mod.failure_from_gate(GateResult("aux_exit", "fail", data), stage="implementation", workload=WORKLOAD)
    assert f.summary == "gate aux_exit fail: cross_accel: test_toydb_cross_accel did not finish within 900s | exit 124"
    assert f.reproduce == 'request_gate("aux_exit")' and f.diagnosis == []  # no crash lines: nothing restated
    report = repair_mod.render_failure(f)
    assert report.index("Thread stacks at the deadline:") < report.index("Log tail:")
    assert report.count("argument_seed=2") == 1 and "Pool::Wait" in report
