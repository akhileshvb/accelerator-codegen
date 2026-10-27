import json
from pathlib import Path

import pytest

from accel_pipeline import repair as repair_mod
from accel_pipeline.backends.codex_exec import CodexExecBackend
from accel_pipeline.config import ModelConfig, PipelineConfig, RepairConfig, SessionConfig
from accel_pipeline.failure import Failure, StageFailure
from accel_pipeline.gates.run import GateResult
from accel_pipeline.repair import RepairCtx, StageStopped, run_with_repair

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
            tpch_db_sf1=tmp_path, tpch_db_sf10=tmp_path, codex_home=tmp_path / "home", sandbox="none",
            write_policy="off", repair=RepairConfig(**repair),
        )
        monkeypatch.setattr(repair_mod, "load_run_workload", lambda cfg, name: {"benchmark_name": "toybench"})
        self.gate_script, self.gate_calls, self.agent_messages, self.events = [], [], [], []
        monkeypatch.setattr(repair_mod, "_run_gates", self._run_gates)
        monkeypatch.setattr(repair_mod, "_rollback", lambda ctx: self.events.append("rollback"))
        monkeypatch.setattr(repair_mod, "_replan", lambda ctx, failure: self.events.append("replan") or False)
        monkeypatch.setattr("accel_pipeline.stages.base.run_agent_stage", self._agent)
        monkeypatch.setattr(repair_mod.time, "sleep", lambda s: None)
        monkeypatch.setattr("accel_pipeline.failure.handle_infra_failure", lambda *a, **k: None)
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

    monkeypatch.setattr("accel_pipeline.stages.base.run_agent_stage", agent)
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

    monkeypatch.setattr("accel_pipeline.stages.base.run_agent_stage", broken)
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
    import accel_pipeline.config as config_mod
    import accel_pipeline.stages.base as base

    world = World(tmp_path, monkeypatch)
    monkeypatch.undo()  # World's stubs are for the unit tests; re-stub what this one needs
    monkeypatch.setattr(repair_mod, "load_run_workload", lambda cfg, name: {"benchmark_name": "toybench"})
    monkeypatch.setattr(repair_mod, "_run_gates", world._run_gates)
    monkeypatch.setattr(repair_mod, "_rollback", lambda ctx: None)
    monkeypatch.setattr(repair_mod, "_replan", lambda ctx, failure: False)
    monkeypatch.setattr("accel_pipeline.failure.handle_infra_failure", lambda *a, **k: None)
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
        "snapshot_frozen": lambda root: {},
        "restore_frozen": lambda root, snap: [],
    }.items():
        monkeypatch.setattr(base, name, value)
    monkeypatch.setattr(config_mod, "resolve_stage_provider", lambda stage, cfg: "codex_exec")
    monkeypatch.setattr("accel_pipeline.prims_library.refresh_for_stage", lambda root: None)
    world.models, world.backend = models_cfg, backend
    return world


def _implement(world):
    from accel_pipeline.session import MAIN
    from accel_pipeline.stages.base import run_agent_stage

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
    from accel_pipeline.quick import QuickResult

    world = World(tmp_path, monkeypatch)
    monkeypatch.undo()  # the real _run_gates, not World's scripted one
    monkeypatch.setattr(repair_mod, "load_run_workload", lambda cfg, name: {"benchmark_name": "toybench"})
    heavy = []
    monkeypatch.setattr(repair_mod, "run_gates_parallel", lambda cfg, names, *a, **k: heavy.append(names) or [])
    monkeypatch.setattr(repair_mod, "run_post_stage_gates_collect", lambda cfg, w, q, names, **k: heavy.append(names) or ([], None))
    verdicts = [QuickResult(status="fail", summary="quick_check: projected full-gate write time ~900 s (budget 450 s)"),
                QuickResult(status="pass")]
    monkeypatch.setattr("accel_pipeline.quick.run_quick_check", lambda *a, **k: verdicts.pop(0))
    ctx = world.ctx(quick_first=True)
    results, failed = repair_mod._run_gates(ctx, ["aux_correctness"])
    assert failed.name == "quick_check" and "projected" in failed.data["reason"] and heavy == []
    repair_mod._run_gates(ctx, ["aux_correctness"])
    assert heavy == [["aux_correctness"]]


def _blocked():
    from accel_pipeline.failure import AGENT_BLOCKER, Failure, StageFailure

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
    monkeypatch.setattr(repair_mod, "_revise_test_suite", lambda ctx: revisions.pop(0))
    run_with_repair(world.ctx(), _blocked, gates=["aux_correctness"])
    assert world.gate_calls == [["aux_correctness"]] and revisions == [False] and world.agent_messages == []


def _out_of_credits():
    from accel_pipeline.failure import Failure, StageFailure
    from accel_pipeline.stages.base import stage_failure

    result = type("R", (), {"error": "unexpected status 402 Payment Required: This request requires more credits",
                            "status": "error", "metadata": {}, "final_text": ""})()
    raise StageFailure(stage_failure("implementation", "codex_exec", result))


def test_an_out_of_credits_provider_stops_at_once_without_retries(world):
    with pytest.raises(StageStopped) as stopped:
        run_with_repair(world.ctx(), _out_of_credits, gates=["aux_correctness"])
    assert "out of credits" in stopped.value.reason and world.agent_messages == [] and world.gate_calls == []
