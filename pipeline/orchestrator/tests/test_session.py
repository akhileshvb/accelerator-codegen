"""One agent turn: session continuity, wall budgets, and the no-progress watchdog (progress clock, footer, redirect,
report_blocker)."""

from __future__ import annotations

import json
import os
import subprocess
import time
from pathlib import Path
from types import SimpleNamespace

import pytest

from accel_pipeline import repair as repair_mod, session as session_mod
from accel_pipeline.codex import StageRunRequest, StageRunResult, CodexExecBackend
from accel_pipeline.config import ModelConfig, NoProgressConfig, PipelineConfig, RepairConfig, SessionConfig
from accel_pipeline.failure import AGENT_BLOCKER, Failure, StageFailure
from accel_pipeline.gates.run import GateResult
from accel_pipeline.repair import RepairCtx, StageStopped, run_with_repair
from accel_pipeline.session import (
    HEARTBEAT_FILE, MAIN, SessionStore, budget_footer, clear_stale_run, context_tokens, keep_alive, run_in_session,
)
from accel_pipeline.stages.base import stage_failure

FAKE_CODEX = Path(__file__).parent / "fixtures" / "fake_codex.sh"
QUERY_FILE = "accelerators/toy/q1/aux.cpp"


class FakeBackend:
    name = "fake"

    def __init__(self, home, session_id="s1"):
        self.home, self.session_id, self.requests = home, session_id, []

    def codex_home(self):
        return self.home

    def run_stage(self, request):
        self.requests.append(request)
        (self.home / "sessions").mkdir(parents=True, exist_ok=True)
        (self.home / "sessions" / f"rollout-x-{self.session_id}.jsonl").write_text("")
        return StageRunResult(backend="fake", stage=request.stage, status="finished",
                              metadata={"session_id": self.session_id})


def _config(**kw):
    return SimpleNamespace(session=SessionConfig(**kw))


def _request(stage="implementation", message="go"):
    return StageRunRequest(stage=stage, instructions="I", user_message=message, agent_name="a", model_id="m")


def test_store_round_trip(tmp_path):
    store = SessionStore(tmp_path)
    assert store.session_id(3, MAIN) == ""
    store.record(3, MAIN, "abc", "implementation", resumed=False)
    store.record(3, "readonly", "def", "audit", resumed=False)
    store.record(3, MAIN, "abc", "crash_debug", resumed=True)
    assert json.loads((tmp_path / "sessions.json").read_text()) == {"3": {"main": "abc", "readonly": "def"}}
    assert SessionStore(tmp_path).session_id(3, MAIN) == "abc"
    assert store.resumes(3, MAIN) == 1 and store.phases(3, MAIN) == ["implementation", "crash_debug"]
    store.reset(3, MAIN)
    assert store.session_id(3, MAIN) == "" and store.session_id(3, "readonly") == "def"


def test_first_run_starts_a_session_then_resumes_it_with_the_message_only(tmp_path):
    backend, config = FakeBackend(tmp_path / "home"), _config()
    run_in_session(backend, _request(), pipeline_config=config, art=tmp_path, query=3, role=MAIN)
    run_in_session(backend, _request("implementation", "again"), pipeline_config=config, art=tmp_path, query=3, role=MAIN)
    first, second = backend.requests
    assert "resume_session_id" not in first.metadata
    assert second.metadata == {"resume_session_id": "s1", "phase_change": False}


def test_a_stage_new_to_the_session_marks_a_phase_change_once(tmp_path):
    backend, config = FakeBackend(tmp_path / "home"), _config()
    for stage in ("implementation", "optimization_worker", "optimization_worker"):
        run_in_session(backend, _request(stage), pipeline_config=config, art=tmp_path, query=3, role=MAIN)
    assert [r.metadata.get("phase_change") for r in backend.requests] == [None, True, False]


def test_roles_do_not_share_sessions(tmp_path):
    config = _config()
    main, reviewer = FakeBackend(tmp_path / "home", "main1"), FakeBackend(tmp_path / "home", "rev1")
    run_in_session(main, _request(), pipeline_config=config, art=tmp_path, query=3, role=MAIN)
    run_in_session(reviewer, _request("audit"), pipeline_config=config, art=tmp_path, query=3, role="readonly")
    assert "resume_session_id" not in reviewer.requests[0].metadata


def test_resume_cap_starts_a_fresh_session_with_a_handoff(tmp_path):
    (tmp_path / "experiment_log.md").write_text("- iter 1: tried a thing\n")
    (tmp_path / "last_failure.md").write_text("gate aux_correctness failed")
    backend, config = FakeBackend(tmp_path / "home"), _config(max_resumes=1)
    for _ in range(3):
        run_in_session(backend, _request(), pipeline_config=config, art=tmp_path, query=3, role=MAIN)
    resumed, fresh = backend.requests[1], backend.requests[2]
    assert resumed.metadata["resume_session_id"] == "s1" and "Handoff" not in resumed.user_message
    assert "resume_session_id" not in fresh.metadata
    assert "Handoff" in fresh.user_message and "tried a thing" in fresh.user_message
    assert "aux_correctness failed" in fresh.user_message


def test_context_cap_triggers_a_handoff(tmp_path):
    home = tmp_path / "home"
    backend, config = FakeBackend(home), _config(max_context_tokens=1000)
    run_in_session(backend, _request(), pipeline_config=config, art=tmp_path, query=3, role=MAIN)
    rollout = home / "sessions" / "rollout-x-s1.jsonl"
    rollout.write_text(json.dumps({"payload": {"type": "token_count", "info": {"last_token_usage": {"input_tokens": 5000}}}}) + "\n")
    assert context_tokens(home, "s1") == 5000
    run_in_session(backend, _request(), pipeline_config=config, art=tmp_path, query=3, role=MAIN)
    assert "Handoff" in backend.requests[1].user_message


def test_a_lost_session_log_falls_back_to_a_fresh_session(tmp_path):
    backend, config = FakeBackend(tmp_path / "home"), _config()
    run_in_session(backend, _request(), pipeline_config=config, art=tmp_path, query=3, role=MAIN)
    (tmp_path / "home" / "sessions" / "rollout-x-s1.jsonl").unlink()
    run_in_session(backend, _request(), pipeline_config=config, art=tmp_path, query=3, role=MAIN)
    assert "resume_session_id" not in backend.requests[1].metadata


def test_continuity_off_runs_without_sessions(tmp_path):
    backend = FakeBackend(tmp_path / "home")
    run_in_session(backend, _request(), pipeline_config=_config(continuity=False), art=tmp_path, query=3, role=MAIN)
    assert not (tmp_path / "sessions.json").exists()


def _run_file(art, started_ago, soft=100, hard=1000):
    (art / "agent_run.json").write_text(json.dumps(
        {"stage": "x", "started": time.time() - started_ago, "soft_deadline_sec": soft, "hard_deadline_sec": hard}))


def test_budget_footer_is_silent_then_asks_to_wrap_up_then_to_checkpoint(tmp_path, monkeypatch):
    monkeypatch.setenv("ACCEL_PIPELINE_ART_DIR", str(tmp_path))
    assert budget_footer(None, "x", 1) == ""
    _run_file(tmp_path, 10)
    assert budget_footer(None, "x", 1) == ""
    _run_file(tmp_path, 150)
    assert "Wrap up" in budget_footer(None, "x", 1)
    _run_file(tmp_path, 950)
    assert "about to be stopped" in budget_footer(None, "x", 1)


def test_a_killed_run_leaves_no_footer_for_the_next_one(tmp_path, monkeypatch):
    monkeypatch.setenv("ACCEL_PIPELINE_ART_DIR", str(tmp_path))
    _run_file(tmp_path, 950)
    clear_stale_run(tmp_path)
    assert budget_footer(None, "x", 1) == ""


def test_keep_alive_beats_while_a_tool_call_blocks_and_stops_after(tmp_path):
    beat = tmp_path / HEARTBEAT_FILE
    with keep_alive(tmp_path, every_sec=0.05):
        time.sleep(0.2)
        assert beat.is_file()
        first = beat.stat().st_mtime
        time.sleep(0.2)
        assert beat.stat().st_mtime > first
    last = beat.stat().st_mtime
    assert time.time() - last > 60  # aged at exit: polling a tool does not read as a running call
    time.sleep(0.2)
    assert beat.stat().st_mtime == last


def test_no_resume_cap_keeps_one_conversation(tmp_path):
    backend, config = FakeBackend(tmp_path / "home"), _config(max_resumes=0)
    for _ in range(30):
        run_in_session(backend, _request(), pipeline_config=config, art=tmp_path, query=3, role=MAIN)
    assert all(r.metadata.get("resume_session_id") == "s1" for r in backend.requests[1:])


def _git_repo(root: Path) -> None:
    for cmd in (["init", "-q"], ["config", "user.email", "t@t"], ["config", "user.name", "t"]):
        subprocess.run(["git", "-C", str(root), *cmd], check=True)
    (root / QUERY_FILE).parent.mkdir(parents=True)
    (root / QUERY_FILE).write_text("v0\n")
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(root), "commit", "-qm", "init"], check=True)


def _repo_config(root: Path, **session) -> PipelineConfig:
    return PipelineConfig(
        repo_root=root, pipeline_root=root / "pipeline", build_dir=root / "build",
        codex_home=root / "home", sandbox="none", write_policy="off", benchmark_name="toy",
        session=SessionConfig(idle_timeout_sec=0, hard_deadline_sec=60, **session))


def _clock(tmp_path, nudge=1.0, redirect=2.0) -> session_mod.ProgressClock:
    _git_repo(tmp_path)
    art = tmp_path / "art"
    art.mkdir()
    return session_mod.ProgressClock(art, tmp_path, query=1, stage="implementation", pipeline_config=_repo_config(tmp_path),
                                     nudge_sec=nudge, redirect_sec=redirect, started_wall=time.time(), sample_sec=0.05)


def _poll_for(clock, seconds):
    end, seen = time.monotonic() + seconds, ""
    while time.monotonic() < end:
        seen = clock.poll() or seen
        time.sleep(0.05)
    return seen


def test_reads_alone_reach_nudge_then_redirect(tmp_path):
    clock = _clock(tmp_path, nudge=0.3, redirect=0.8)
    assert _poll_for(clock, 0.5) == "nudge"
    assert _poll_for(clock, 0.6) == "redirect"


def test_editing_an_owned_file_resets_the_clock(tmp_path):
    clock = _clock(tmp_path, nudge=0.4, redirect=0.8)
    for i in range(12):
        (tmp_path / QUERY_FILE).write_text(f"v{i + 1}\n")
        assert _poll_for(clock, 0.1) == ""


def test_edits_outside_the_stage_ownership_are_not_progress(tmp_path):
    clock = _clock(tmp_path, nudge=0.3, redirect=0.8)
    (tmp_path / "accelerators/toy/q2").mkdir()
    for i in range(8):
        (tmp_path / "accelerators/toy/q2/f.cpp").write_text(f"{i}\n")
        _poll_for(clock, 0.1)
    assert clock.poll() == "redirect"


def test_stage_artifact_is_progress(tmp_path):
    clock = _clock(tmp_path, nudge=0.4, redirect=0.8)
    for i in range(8):
        (tmp_path / "art" / "analysis.json").write_text(str(i))
        assert _poll_for(clock, 0.1) == ""


def test_changed_check_result_resets_but_the_same_one_does_not(tmp_path):
    clock = _clock(tmp_path, nudge=0.4, redirect=5)
    art = tmp_path / "art"
    fail = lambda i: json.dumps({"status": "fail", "summary": f"build error {i}"})
    session_mod.record_check(art, "quick_check", (), fail(0))
    assert _poll_for(clock, 0.2) == ""  # a new result: progress
    reset_at = clock.last_progress
    for i in range(1, 8):  # the same failure again (only a number differs)
        session_mod.record_check(art, "quick_check", (), fail(i))
        _poll_for(clock, 0.1)
    assert clock.last_progress == reset_at and clock.poll() == "nudge"
    session_mod.record_check(art, "quick_check", (), json.dumps({"status": "pass", "summary": "ok"}))
    _poll_for(clock, 0.15)
    assert clock.poll() == "" and clock.last_progress > reset_at


def test_time_inside_a_tool_call_is_not_idle_time(tmp_path):
    clock = _clock(tmp_path, nudge=0.4, redirect=0.8)
    for _ in range(20):
        (tmp_path / "art" / "agent_heartbeat").touch()
        assert clock.poll() == ""
        time.sleep(0.05)


def test_footer_asks_for_the_blocker_after_nudge_and_counts_guarded_reads(tmp_path, monkeypatch):
    art = tmp_path
    monkeypatch.setenv("ACCEL_PIPELINE_ART_DIR", str(art))
    (art / "agent_run.json").write_text(json.dumps({"started": time.time(), "no_progress": {"nudge_sec": 60}}))
    (art / "agent_progress.json").write_text(json.dumps({"last_progress": time.time() - 30}))
    assert budget_footer(None, "x", 1) == ""
    (art / "agent_progress.json").write_text(json.dumps({"last_progress": time.time() - 600, "guarded_reads": 11}))
    footer = budget_footer(None, "x", 1)
    assert "No progress for 10 min" in footer and "report_blocker" in footer and "11 shell reads" in footer


def _backend(tmp_path, monkeypatch, mode, nudge=0.3, redirect=0.8, exit_gate=()):
    _git_repo(tmp_path)
    monkeypatch.setenv("FAKE_CODEX_MODE", mode)
    monkeypatch.setenv("FAKE_CODEX_DIR", str(tmp_path))
    monkeypatch.setenv("FAKE_CODEX_EDIT", str(tmp_path / QUERY_FILE))
    monkeypatch.setenv("FAKE_CODEX_ART", str(tmp_path / "art"))
    (tmp_path / "art").mkdir()
    cfg = _repo_config(tmp_path, no_progress=NoProgressConfig(nudge_sec=nudge, redirect_sec=redirect), exit_gate=exit_gate)
    models = ModelConfig(api_key_env="K", presets={}, stages={}, codex_bin=str(FAKE_CODEX))
    return CodexExecBackend(pipeline_config=cfg, model_config=models, query=1, stage="implementation",
                            art_dir=tmp_path / "art")



def test_a_reading_agent_is_redirected_with_a_directed_failure(tmp_path, monkeypatch):
    backend = _backend(tmp_path, monkeypatch, "sleep")
    (tmp_path / "art" / "last_failure.md").write_text("## code failure\nlinker error")
    result = backend.run_stage(_request())
    assert result.metadata["stop_reason"] == "no_progress" and "no progress" in result.error
    failure = stage_failure("implementation", "codex_exec", result)
    assert failure.kind == "timeout" and failure.source == "no_progress"
    assert "report_blocker" in failure.diagnosis[0] and "request_structural_reanalysis" in failure.diagnosis[0]
    assert "linker error" in failure.evidence and "no file of yours changed" in failure.evidence


def test_an_editing_agent_is_not_redirected(tmp_path, monkeypatch):
    result = _backend(tmp_path, monkeypatch, "edit").run_stage(_request())
    assert result.status == "finished"


def test_read_counter_counts_only_guarded_shell_reads_since_the_turn_started(tmp_path):
    rollout = tmp_path / "r.jsonl"
    old = '{"timestamp":"2000-01-01T00:00:00.000Z","payload":{"type":"function_call","name":"exec_command","arguments":"{\\"cmd\\":\\"cat pipeline/gates/a.py\\"}"}}\n'
    now = time.strftime("%Y-%m-%dT%H:%M:%S.000Z", time.gmtime())
    line = lambda cmd: ('{"timestamp":"%s","payload":{"type":"function_call","name":"exec_command","arguments":"{\\"cmd\\":\\"%s\\"}"}}\n' % (now, cmd))
    rollout.write_text(old + line("rg x pipeline/orchestrator/accel_pipeline") + line("cat accelerators/toy/q1/aux.cpp")
                       + line("ls scripts/") + line("cat pipeline/clean_tree/a.py"))
    counter = session_mod._ReadCounter(time.time() - 60)
    counter.update(rollout)
    assert counter.count == 3


@pytest.mark.parametrize("mode", ["blocker", "blocker_exit"])  # blocker_exit: the agent stops at once, as the tool says
def test_report_blocker_ends_the_turn_and_becomes_an_agent_blocker_failure(tmp_path, monkeypatch, mode):
    result = _backend(tmp_path, monkeypatch, mode, nudge=0, redirect=0).run_stage(_request())
    assert result.metadata["stop_reason"] == "blocker"
    failure = stage_failure("implementation", "codex_exec", result)
    assert failure.kind == "infra" and failure.infra_reason == AGENT_BLOCKER and "summary" in failure.evidence


def test_the_mcp_report_blocker_tool_writes_blocker_md(tmp_path):
    from accel_pipeline.mcp_server import build_mcp_tool_callables

    tools = build_mcp_tool_callables(_repo_config(tmp_path), {}, "w", 1, "implementation", 0, tmp_path)
    assert "Stop now" in tools["report_blocker"]("the gate is broken", "traceback ...")
    assert "the gate is broken" in (tmp_path / "blocker.md").read_text()


# ---- the repair loop ----

class World:
    def __init__(self, tmp_path, monkeypatch):
        self.config = PipelineConfig(
            repo_root=tmp_path, pipeline_root=tmp_path / "pipeline", build_dir=tmp_path / "build",
            codex_home=tmp_path / "home", sandbox="none", write_policy="off",
            repair=RepairConfig())
        monkeypatch.setattr(repair_mod, "load_run_workload", lambda cfg, name: {"benchmark_name": "toy"})
        self.gate_calls, self.agent_calls = [], 0
        self.agent = self._blocker_agent
        monkeypatch.setattr(repair_mod, "_run_gates", self._gates)
        monkeypatch.setattr(repair_mod, "_rollback", lambda ctx: None)
        monkeypatch.setattr(repair_mod, "_replan", lambda ctx, failure: False)
        monkeypatch.setattr("accel_pipeline.stages.base.run_agent_stage", lambda **kw: self.agent())
        self.gates_pass_from = 2

    def ctx(self):
        return RepairCtx(self.config, None, "w", 3, stage="implementation", change_kind="implementation")

    def _gates(self, ctx, names):
        self.gate_calls.append(names)
        if len(self.gate_calls) >= self.gates_pass_from:
            return [GateResult("g", "pass", {})], None
        bad = GateResult("g", "fail", {"log_tail": "boom in accelerators/toy/q3/a.cpp", "returncode": 1})
        return [bad], bad

    def _blocker_agent(self):
        self.agent_calls += 1
        raise StageFailure(Failure(kind="infra", stage="crash_debug", source="agent", infra_reason=AGENT_BLOCKER,
                                   summary="the agent reported a blocker (blocker.md)", evidence="gate is broken"))


def test_a_blocker_from_a_repair_turn_is_rechecked_and_passes(tmp_path, monkeypatch):
    world = World(tmp_path, monkeypatch)
    run_with_repair(world.ctx(), lambda: None, gates=["g"])
    assert world.agent_calls == 1 and len(world.gate_calls) == 2


def test_a_repeated_blocker_is_adapted_to_until_the_budget_is_spent_then_cited(tmp_path, monkeypatch):
    world = World(tmp_path, monkeypatch)
    world.gates_pass_from = 99
    with pytest.raises(StageStopped) as stopped:
        run_with_repair(world.ctx(), lambda: None, gates=["g"])
    assert world.agent_calls == world.config.repair.max_code_rounds
    report = stopped.value.report.read_text()
    assert "the agent's blocker" in report and "gate is broken" in report and "still failed" in report


def test_a_pass_of_the_stages_exit_gate_ends_the_turn_as_a_finish(tmp_path, monkeypatch):
    backend = _backend(tmp_path, monkeypatch, "gate_pass", nudge=0, redirect=0, exit_gate=(("implementation", "fake_gate"),))
    started = time.monotonic()
    result = backend.run_stage(_request())
    assert result.status == "finished" and time.monotonic() - started < 20
    assert not result.metadata.get("timed_out")


def test_a_failing_exit_gate_or_a_stage_without_one_runs_to_the_end(tmp_path, monkeypatch):
    result = _backend(tmp_path, monkeypatch, "gate_fail", nudge=0, redirect=0, exit_gate=(("implementation", "fake_gate"),)).run_stage(_request())
    assert result.status == "finished" and result.metadata["exit_code"] == 0
    (tmp_path / "o").mkdir()
    other = _backend(tmp_path / "o", monkeypatch, "gate_fail", nudge=0, redirect=0, exit_gate=(("test_suite", "fake_gate"),))
    assert other.run_stage(_request()).status == "finished"


def test_an_exit_gate_record_older_than_the_turn_does_not_end_it(tmp_path, monkeypatch):
    backend = _backend(tmp_path, monkeypatch, "gate_fail", nudge=0, redirect=0, exit_gate=(("implementation", "fake_gate"),))
    gates = tmp_path / "art" / "gates"
    gates.mkdir()
    (gates / "fake_gate.json").write_text('{"status": "pass"}')
    os.utime(gates / "fake_gate.json", (1, 1))
    assert backend._exit_gate_passed(time.time()) is False


def test_the_mcp_validate_analysis_tool_reports_the_stage_check(tmp_path):
    from accel_pipeline.mcp_server import build_mcp_tool_callables

    tools = build_mcp_tool_callables(_repo_config(tmp_path), {}, "w", 1, "analysis", 0, tmp_path)
    assert tools["validate_analysis"]().startswith("invalid: analysis file not found")


def test_the_mcp_update_analysis_tool_patches_then_validates(tmp_path, monkeypatch):
    import json

    from accel_pipeline.mcp_server import build_mcp_tool_callables

    target = tmp_path / "analysis.json"
    target.write_text(json.dumps({"x": {"y": 1}}))
    monkeypatch.setattr("accel_pipeline.config.analysis_artifact_path", lambda *a, **k: target)
    tools = build_mcp_tool_callables(_repo_config(tmp_path), {}, "w", 1, "analysis", 0, tmp_path)
    out = tools["update_analysis"]([{"op": "replace", "path": "/x/y", "value": 2}])
    assert out.startswith("applied 1 operation(s); validation: invalid") and json.loads(target.read_text()) == {"x": {"y": 2}}
    assert tools["update_analysis"]([{"op": "remove", "path": "/nope"}]).startswith("not applied")
    assert json.loads(target.read_text()) == {"x": {"y": 2}}


def _data_call(art, error=None):
    with (art / "data_queries.jsonl").open("a") as log:
        log.write(json.dumps({"tool": "query_data", "error": error}) + "\n")


def test_successful_data_calls_are_progress_in_the_analysis_stages_only(tmp_path):
    clock = _clock(tmp_path, nudge=0.4, redirect=0.8)
    clock.stage = "analysis"
    for _ in range(8):
        _data_call(tmp_path / "art")
        assert _poll_for(clock, 0.1) == ""
    for _ in range(8):  # failed calls are not
        _data_call(tmp_path / "art", error="syntax")
        _poll_for(clock, 0.1)
    assert clock.poll() == "redirect"


def test_data_calls_are_not_progress_while_implementing(tmp_path):
    clock = _clock(tmp_path, nudge=0.3, redirect=0.8)
    for _ in range(8):
        _data_call(tmp_path / "art")
        _poll_for(clock, 0.1)
    assert clock.poll() == "redirect"


NARROWED = {"run_pipeline_command"}  # the agent passes the command only, never the cwd or timeout


def test_every_mcp_proxy_takes_the_parameters_of_the_tool_it_forwards_to(tmp_path):
    import inspect

    from accel_pipeline.mcp_server import build_mcp_tool_callables, register_core

    proxies = {}
    register_core(lambda read_only: lambda fn: proxies.setdefault(fn.__name__, fn), lambda *args: "")
    tools = build_mcp_tool_callables(_repo_config(tmp_path), {}, "w", 1, "optimization", 0, tmp_path)
    params = lambda fn: list(inspect.signature(fn).parameters)
    drift = {name: (params(proxy), params(tools[name])) for name, proxy in proxies.items()
             if name in tools and name not in NARROWED and params(proxy) != params(tools[name])}
    assert drift == {}
