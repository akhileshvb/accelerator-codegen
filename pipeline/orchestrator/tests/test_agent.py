"""One agent turn: the codex step with its config, sidecar and heartbeat, session continuity, wall budgets, the
no-progress watchdog (progress clock, footer, redirect, report_blocker) and the sandbox the step runs in."""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import time
from pathlib import Path
from types import SimpleNamespace

import pytest

from accel_pipeline import agent, layout, repair as repair_mod
from accel_pipeline.agent import (
    budget_footer,
    clear_stale_run,
    CodexExecBackend,
    context_tokens,
    keep_alive,
    MAIN,
    run_in_session,
    SessionStore,
    StageRunRequest,
    StageRunResult,
)
from accel_pipeline.config import (
    HEARTBEAT_FILE,
    load_model_config,
    ModelConfig,
    NoProgressConfig,
    PipelineConfig,
    RepairConfig,
    SessionConfig,
)
from accel_pipeline.gates import GateResult
from accel_pipeline.layout import StageWriteCheck, violation_message
from accel_pipeline.repair import (
    AGENT_BLOCKER,
    Failure,
    RepairCtx,
    run_with_repair,
    StageFailure,
    StageStopped,
)
from accel_pipeline.stages import stage_failure


REPO_ROOT = Path(__file__).resolve().parents[3]


def _pipeline_config(**overrides) -> PipelineConfig:
    return PipelineConfig(
        repo_root=REPO_ROOT,
        pipeline_root=REPO_ROOT / "pipeline",
        build_dir=REPO_ROOT / "build-release",
        **overrides,
    )


def test_ensure_codex_config_materializes_template_once(tmp_path):
    from accel_pipeline.agent import ensure_codex_config

    config = tmp_path / "home" / "config.toml"
    assert ensure_codex_config(config) == config
    text = config.read_text(encoding="utf-8")
    assert f'command = "{REPO_ROOT}/pipeline/codex/run_mcp_server.sh"' in text
    assert "{{" not in text
    config.write_text("custom", encoding="utf-8")
    ensure_codex_config(config)
    assert config.read_text(encoding="utf-8") == "custom"


def test_mcp_env_sidecar_quotes_values_with_spaces(monkeypatch, tmp_path):
    from accel_pipeline.agent import sidecar_env_line
    from accel_pipeline.config import load_pipeline_config

    discovery = (
        "postgres: docker container=accel_pg_test 127.0.0.1:5434 "
        "so=/usr/lib/postgresql/17/lib/accelerator_codegen_pg.so"
    )
    monkeypatch.setenv("ACCEL_PG_DISCOVERY", discovery)
    line = sidecar_env_line("ACCEL_PG_DISCOVERY", discovery)
    assert " " not in line.split("=", 1)[0]
    assert line.startswith("ACCEL_PG_DISCOVERY=")
    assert "docker" in line

    pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
    model = load_model_config(REPO_ROOT / "pipeline" / "config" / "models.yaml", repo_root=REPO_ROOT)
    backend = CodexExecBackend(
        pipeline_config=pipeline_config,
        model_config=model,
        query=0,
        stage="impl_epochs",
        workload_name="engine_postgres",
    )
    path = backend._write_mcp_env_sidecar(tmp_path)
    text = path.read_text(encoding="utf-8")
    assert "ACCEL_PG_DISCOVERY=" in text
    proc = __import__("subprocess").run(
        [
            "bash",
            "-c",
            "set -euo pipefail; set -a; source \"$1\"; set +a; "
            "test -n \"$ACCEL_PG_DISCOVERY\"; "
            "test \"$ACCEL_PIPELINE_QUERY\" = \"0\"; "
            "printf '%s' \"$ACCEL_PG_DISCOVERY\"",
            "source_sidecar",
            str(path),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert proc.returncode == 0, proc.stderr
    assert "docker container=accel_pg_test" in proc.stdout


def _fake_backend(tmp_path, monkeypatch, *, mode="ok", idle=0, hard=60, art=True):
    from accel_pipeline.config import SessionConfig

    monkeypatch.setenv("FAKE_CODEX_MODE", mode)
    monkeypatch.setenv("FAKE_CODEX_DIR", str(tmp_path))
    config = PipelineConfig(
        repo_root=tmp_path, pipeline_root=tmp_path / "pipeline", build_dir=tmp_path / "build",
        codex_home=tmp_path / "home", sandbox="none",
        write_policy="off", session=SessionConfig(idle_timeout_sec=idle, hard_deadline_sec=hard),
    )
    models = ModelConfig(api_key_env="K", presets={}, stages={}, codex_bin=str(FAKE_CODEX))
    return CodexExecBackend(pipeline_config=config, model_config=models, query=1, stage="implementation",
                            art_dir=tmp_path / "art" if art else None)


def _codex_request(message="do it", **metadata):
    from accel_pipeline.agent import StageRunRequest

    return StageRunRequest(stage="implementation", instructions="INSTRUCTIONS", user_message=message,
                           agent_name="a", model_id="m", metadata=metadata)


def test_fake_codex_finishes_and_reports_its_session(tmp_path, monkeypatch):
    result = _fake_backend(tmp_path, monkeypatch).run_stage(_codex_request())
    assert result.status == "finished"
    assert result.metadata["session_id"] == "11111111-2222-3333-4444-555555555555"
    assert "INSTRUCTIONS" in (tmp_path / "prompts.log").read_text()


def test_idle_timeout_kills_a_silent_codex_and_keeps_its_session(tmp_path, monkeypatch):
    result = _fake_backend(tmp_path, monkeypatch, mode="sleep", idle=1).run_stage(_codex_request())
    assert result.status == "error" and result.metadata["timed_out"]
    assert "idle" in result.error
    assert result.metadata["session_id"] == "11111111-2222-3333-4444-555555555555"
    assert result.metadata["duration_ms"] < 15_000


def test_hard_deadline_kills_even_a_busy_codex(tmp_path, monkeypatch):
    result = _fake_backend(tmp_path, monkeypatch, mode="chatty", idle=30, hard=1).run_stage(_codex_request())
    assert result.metadata["timed_out"] and "hard deadline" in result.error


def test_activity_defers_the_idle_timeout(tmp_path, monkeypatch):
    result = _fake_backend(tmp_path, monkeypatch, mode="chatty", idle=2).run_stage(_codex_request())
    assert result.status == "finished"


def test_heartbeat_counts_as_activity(tmp_path, monkeypatch):
    import threading
    import time

    from accel_pipeline.agent import heartbeat

    stop = threading.Event()
    (tmp_path / "art").mkdir()

    def beat():
        while not stop.is_set():
            heartbeat(tmp_path / "art")
            time.sleep(0.2)

    thread = threading.Thread(target=beat)
    thread.start()
    try:
        monkeypatch.setenv("FAKE_CODEX_MODE", "chatty")
        backend = _fake_backend(tmp_path, monkeypatch, mode="sleep", idle=1, hard=3)
        result = backend.run_stage(_codex_request())
    finally:
        stop.set()
        thread.join()
    assert "hard deadline" in result.error  # silent for 3s, never idle: only the hard deadline ended it


def test_resume_sends_only_the_message_and_continues_the_session(tmp_path, monkeypatch):
    sid = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
    _fake_backend(tmp_path, monkeypatch).run_stage(_codex_request("fix the gate", resume_session_id=sid))
    args = (tmp_path / "args.log").read_text()
    assert "exec resume" in args and sid in args
    prompt = (tmp_path / "prompts.log").read_text()
    assert "fix the gate" in prompt and "INSTRUCTIONS" not in prompt


def test_phase_change_sends_the_instructions_once_under_a_header(tmp_path, monkeypatch):
    sid = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
    _fake_backend(tmp_path, monkeypatch).run_stage(_codex_request("next", resume_session_id=sid, phase_change=True))
    prompt = (tmp_path / "prompts.log").read_text()
    assert "NEW PHASE" in prompt and "INSTRUCTIONS" in prompt and "next" in prompt


def test_agent_run_file_marks_the_run_for_the_budget_footer(tmp_path, monkeypatch):
    (tmp_path / "art").mkdir()
    backend = _fake_backend(tmp_path, monkeypatch, mode="chatty")
    seen = []
    original = backend._supervise

    def spy(*args, **kwargs):
        import threading

        threading.Timer(0.5, lambda: seen.append((tmp_path / "art" / "agent_run.json").is_file())).start()
        return original(*args, **kwargs)

    backend._supervise = spy
    backend.run_stage(_codex_request())
    assert seen == [True] and not (tmp_path / "art" / "agent_run.json").exists()


def test_query_stages_get_the_artifact_dir_so_their_progress_clock_runs(monkeypatch, tmp_path):
    """analysis and test_suite are watched like the implementation stages: the backend knows the query's artifact dir."""
    from types import SimpleNamespace

    from accel_pipeline import agent

    monkeypatch.setattr(agent, "load_run_workload", lambda cfg, name: "wl")
    monkeypatch.setattr(agent, "query_artifacts_dir", lambda root, q, workload=None: tmp_path / f"q{q}")
    cfg = SimpleNamespace(repo_root=tmp_path, sandbox="none")
    for stage in ("analysis", "test_suite", "implementation", "audit"):
        b = agent.create_agent_backend(provider="codex_exec", pipeline_config=cfg, query=3, stage=stage,
                                       model_config=SimpleNamespace(), workload_name="w")
        assert b._art_dir == tmp_path / "q3", stage
    for stage in ("host_bootstrap", "validation", "architecture_audit"):
        b = agent.create_agent_backend(provider="codex_exec", pipeline_config=cfg, query=3, stage=stage,
                                       model_config=SimpleNamespace(), workload_name="w")
        assert b._art_dir is None, stage


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


def _clock(tmp_path, nudge=1.0, redirect=2.0) -> agent.ProgressClock:
    _git_repo(tmp_path)
    art = tmp_path / "art"
    art.mkdir()
    return agent.ProgressClock(art, tmp_path, query=1, stage="implementation", pipeline_config=_repo_config(tmp_path),
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
    agent.record_check(art, "quick_check", (), fail(0))
    assert _poll_for(clock, 0.2) == ""  # a new result: progress
    reset_at = clock.last_progress
    for i in range(1, 8):  # the same failure again (only a number differs)
        agent.record_check(art, "quick_check", (), fail(i))
        _poll_for(clock, 0.1)
    assert clock.last_progress == reset_at and clock.poll() == "nudge"
    agent.record_check(art, "quick_check", (), json.dumps({"status": "pass", "summary": "ok"}))
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
    counter = agent._ReadCounter(time.time() - 60)
    counter.update(rollout)
    assert counter.count == 3


@pytest.mark.parametrize("mode", ["blocker", "blocker_exit"])  # blocker_exit: the agent stops at once, as the tool says
def test_report_blocker_ends_the_turn_and_becomes_an_agent_blocker_failure(tmp_path, monkeypatch, mode):
    result = _backend(tmp_path, monkeypatch, mode, nudge=0, redirect=0).run_stage(_request())
    assert result.metadata["stop_reason"] == "blocker"
    failure = stage_failure("implementation", "codex_exec", result)
    assert failure.kind == "infra" and failure.infra_reason == AGENT_BLOCKER and "summary" in failure.evidence


def test_the_mcp_report_blocker_tool_writes_blocker_md(tmp_path):
    from accel_pipeline.mcp import build_mcp_tool_callables

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
        monkeypatch.setattr("accel_pipeline.stages.run_agent_stage", lambda **kw: self.agent())
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
    from accel_pipeline.mcp import build_mcp_tool_callables

    tools = build_mcp_tool_callables(_repo_config(tmp_path), {}, "w", 1, "analysis", 0, tmp_path)
    assert tools["validate_analysis"]().startswith("invalid: analysis file not found")


def test_the_mcp_update_analysis_tool_patches_then_validates(tmp_path, monkeypatch):
    import json

    from accel_pipeline.mcp import build_mcp_tool_callables

    target = tmp_path / "analysis.json"
    target.write_text(json.dumps({"x": {"y": 1}}))
    monkeypatch.setattr("accel_pipeline.mcp.analysis_artifact_path", lambda *a, **k: target)
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

    from accel_pipeline.mcp import build_mcp_tool_callables, register_core

    proxies = {}
    register_core(lambda read_only: lambda fn: proxies.setdefault(fn.__name__, fn), lambda *args: "")
    tools = build_mcp_tool_callables(_repo_config(tmp_path), {}, "w", 1, "optimization", 0, tmp_path)
    params = lambda fn: list(inspect.signature(fn).parameters)
    drift = {name: (params(proxy), params(tools[name])) for name, proxy in proxies.items()
             if name in tools and name not in NARROWED and params(proxy) != params(tools[name])}
    assert drift == {}


PRIM_A, PRIM_B = f"{layout.PRIMS}/toy_a.h", f"{layout.PRIMS}/toy_b.h"
PROOF = f"{layout.PROOFLIB}/toy_lib.rs"
NEEDS: dict = {}  # engine name -> the backend `info` the sandbox would read
needs_bwrap = pytest.mark.skipif(not shutil.which("bwrap"), reason="bubblewrap not installed")


def _cfg(root: Path, engine: str = "toydb", **kw) -> SimpleNamespace:
    base = dict(repo_root=root, build_dir=root / "build-x", codex_home=None, engine_name=engine, datasets={},
                workload_kind="aux_state", benchmark_name="toybench", sandbox_extra_ro=[], sandbox_extra_rw=[],
                write_paths=["accelerators/*/q*/**", "pipeline/artifacts/**"], shared_paths=[])
    base.update(kw)
    return SimpleNamespace(**base)


def _tree(root: Path) -> Path:
    for d in ("substrate", "workloads/toybench", "accelerators/toybench/q6", "accelerators/toybench/q3", "backends/toydb",
              "backends/dockerdb", "pipeline"):
        (root / d).mkdir(parents=True, exist_ok=True)
    (root / "backends/toydb/host.sh").write_text("#!/usr/bin/env bash\n# embedded\n")
    (root / "backends/dockerdb/host.sh").write_text("#!/usr/bin/env bash\ndocker exec x run\n")
    for h in ("toydb", "dockerdb"):
        (root / f"backends/{h}/host.sh").chmod(0o755)
    return root


@pytest.fixture
def isolated(monkeypatch: pytest.MonkeyPatch, tmp_path: Path):
    sock = tmp_path / "docker.sock"
    sock.touch()
    monkeypatch.setattr(agent, "DOCKER_SOCKET", sock)
    for k in ("ACCEL_PIPELINE_PRIMARY_REPO", "ACCEL_ENGINE_STATE_DIR", "ACCEL_DATASET_DIR",
              "ACCEL_PIPELINE_PACK_QUERIES", "CCACHE_DIR"):
        monkeypatch.delenv(k, raising=False)
    NEEDS.clear()
    monkeypatch.setattr(agent, "backend_needs", lambda cfg: NEEDS.get(cfg.engine_name))
    return sock


def _dsts(binds, mode=None):
    return {dst for m, _, dst in binds if mode in (None, m)}


def test_cpp_mounts_tree_layers_build_data(tmp_path: Path, isolated: Path) -> None:
    root = _tree(tmp_path / "tree").resolve()
    data = tmp_path / "data"
    data.mkdir()
    (data / "toybench.toydb").touch()
    extra = tmp_path / "extra"
    extra.mkdir()
    cfg = _cfg(root, datasets={"toybench_sf1": data / "toybench.toydb"}, build_dir=tmp_path / "scratch" / "build",
               sandbox_extra_ro=[str(extra)])
    binds = agent.cpp_mounts(cfg)
    order = [(m, d) for m, _, d in binds]
    assert order.index(("rw", root)) < order.index(("ro", root / "substrate")) < len(order)
    assert ("ro", root / "workloads") in order
    assert ("rw", (tmp_path / "scratch" / "build").resolve()) in order  # build dir outside the tree
    assert ("ro", data.resolve()) in order and ("ro", extra.resolve()) in order
    assert not any(m == "rw" and d != root and not d.is_relative_to(tmp_path) for m, d in order
                   if d not in {Path.home() / ".cache" / "ccache"})  # nothing else writable
    # a build dir inside the tree adds no bind
    inside = agent.cpp_mounts(_cfg(root, build_dir=root / "build-y"))
    assert ("rw", root / "build-y") not in [(m, d) for m, _, d in inside]


def test_worktree_mode_mounts_primary_and_hides_other_workers(tmp_path: Path, isolated: Path,
                                                              monkeypatch: pytest.MonkeyPatch) -> None:
    primary = _tree(tmp_path / "primary").resolve()
    wt = primary / "pipeline" / "worktrees" / "w1"
    _tree(wt)
    monkeypatch.setenv("ACCEL_PIPELINE_PRIMARY_REPO", str(primary))
    cfg = _cfg(wt, build_dir=primary / "build-release-w1", codex_home=primary / "pipeline/.codex/workers/w1")
    order = [(m, d) for m, _, d in agent.cpp_mounts(cfg)]
    wts = primary / "pipeline" / "worktrees"
    assert order.index(("rw", primary)) < order.index(("tmpfs", wts)) < order.index(("rw", wt.resolve()))
    for tree in (primary, wt.resolve()):
        assert ("ro", tree / "substrate") in order and ("ro", tree / "workloads") in order
        assert order.index(("rw", tree)) < order.index(("ro", tree / "substrate"))
    # worker build dir and codex home live in the primary: already covered, no separate binds
    assert ("rw", primary / "build-release-w1") not in order


@needs_bwrap
def test_bwrap_cpp_sandbox_blocks_substrate_and_allows_the_query_dir(tmp_path: Path, isolated: Path,
                                                                     monkeypatch: pytest.MonkeyPatch) -> None:
    root = _tree(tmp_path / "tree").resolve()
    cfg = _cfg(root, benchmark_name="toybench")
    monkeypatch.setenv("ACCEL_PIPELINE_QUERY", "6")
    prefix = agent.cpp_prefix(cfg)
    script = ("touch substrate/x 2>/dev/null && echo SUB_WRITABLE; touch workloads/toybench/x 2>/dev/null && echo WL_WRITABLE; "
              "touch accelerators/toybench/q6/ok && echo QUERY_OK; touch build-x/ok && echo BUILD_OK; ls /home | wc -l; "
              "test -d /sys/devices/system/cpu && echo SYS_VISIBLE; touch /sys/x 2>/dev/null && echo SYS_WRITABLE")
    proc = subprocess.run(prefix + ["bash", "-c", script], capture_output=True, text=True, timeout=60)
    out = proc.stdout.split()
    assert "QUERY_OK" in out and "BUILD_OK" in out, proc.stderr
    assert "SUB_WRITABLE" not in out and "WL_WRITABLE" not in out
    assert "SYS_VISIBLE" in out and "SYS_WRITABLE" not in out  # without /sys, perf finds no PMU and every profile reads "not applicable"
    assert (root / "accelerators/toybench/q6/ok").exists() and not (root / "substrate/x").exists()
    check = agent.cpp_selfcheck(cfg, 6)
    assert check["problems"] in ([], ["MISSING codex"]), check
    assert check["notes"] and "no usable host.sh" in check["notes"][0]  # the fake engine has no info


# ---- the post-stage ownership check ----

def _git_tree(root: Path) -> Path:
    _tree(root)
    for f in ("substrate/s.h", "accelerators/toybench/q6/k.cpp", "accelerators/toybench/q3/k.cpp", "backends/toydb/e.cpp",
              PRIM_A, PROOF):
        (root / f).parent.mkdir(parents=True, exist_ok=True)
        (root / f).write_text("x\n")
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(root), "-c", "user.email=a@b", "-c", "user.name=t", "commit", "-qm", "init"],
                   check=True)
    return root


def _violations(root: Path, write, **kw) -> dict[str, str]:
    check = StageWriteCheck(root, query=6, stage=kw.pop("stage", "implementation"),
                            pipeline_config=kw.pop("cfg", _cfg(root)))
    write(root)
    return dict(check.violations())


def test_ownership_allows_own_query_and_artifacts(tmp_path: Path) -> None:
    root = _git_tree(tmp_path / "t")

    def write(r: Path) -> None:
        (r / "accelerators/toybench/q6/k.cpp").write_text("changed\n")
        (r / "accelerators/toybench/q6/new.cpp").write_text("new\n")
        (r / "pipeline/artifacts/toybench/q6").mkdir(parents=True)
        (r / "pipeline/artifacts/toybench/q6/a.json").write_text("{}")

    assert _violations(root, write) == {}


def test_ownership_flags_foreign_layers(tmp_path: Path) -> None:
    root = _git_tree(tmp_path / "t")

    def write(r: Path) -> None:
        (r / "accelerators/toybench/q3/k.cpp").write_text("peer edit\n")
        (r / "backends/toydb/e.cpp").write_text("backend edit\n")
        (r / PRIM_A).write_text("prim edit\n")
        (r / PROOF).write_text("proof edit\n")
        (r / "substrate/s.h").write_text("substrate edit\n")
        (r / "accelerators/toybench/q6/k.cpp").write_text("mine\n")

    bad = _violations(root, write)
    assert set(bad) == {"accelerators/toybench/q3/k.cpp", "backends/toydb/e.cpp", PRIM_A,
                        PROOF, "substrate/s.h"}
    assert "cross-query" in bad["accelerators/toybench/q3/k.cpp"] and "prooflib_publish" in bad[PROOF]
    assert "never writable" in bad["substrate/s.h"] or "SUBSTRATE" in bad["substrate/s.h"].upper()
    msg = violation_message("implementation", list(bad.items()))
    assert "undo" in msg and "backends/toydb/e.cpp" in msg


def test_ownership_ignores_what_was_dirty_before_the_stage(tmp_path: Path) -> None:
    root = _git_tree(tmp_path / "t")
    (root / "accelerators/toybench/q3/k.cpp").write_text("dirty before\n")
    assert _violations(root, lambda r: None) == {}


def test_ownership_exempts_publish_tool_writes(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    import json
    import time

    root = _git_tree(tmp_path / "t")

    def write(r: Path) -> None:
        (r / PRIM_A).write_text("published\n")
        (r / PRIM_B).write_text("not published\n")
        (r / "pipeline/prims_provenance.jsonl").write_text(
            json.dumps({"ts": time.time() + 1, "files": [PRIM_A]}) + "\n")

    assert set(_violations(root, write)) == {PRIM_B}


def test_ownership_engine_backend_run_owns_its_backend(tmp_path: Path) -> None:
    root = _git_tree(tmp_path / "t")
    cfg = _cfg(root, workload_kind="engine_backend")

    def write(r: Path) -> None:
        (r / "backends/toydb/e.cpp").write_text("mine\n")
        (r / "backends/dockerdb/x.cpp").write_text("other engine\n")

    assert set(_violations(root, write, cfg=cfg, stage="impl_runtime")) == {"backends/dockerdb/x.cpp"}


def test_ownership_shared_tree_pack_skips_other_queries(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    root = _git_tree(tmp_path / "t").resolve()
    monkeypatch.setenv("ACCEL_PIPELINE_PACK_QUERIES", "3,6")
    monkeypatch.setenv("ACCEL_PIPELINE_PRIMARY_REPO", str(root))

    def write(r: Path) -> None:
        (r / "accelerators/toybench/q3/k.cpp").write_text("another worker's edit\n")
        (r / "backends/toydb/e.cpp").write_text("backend edit\n")

    assert set(_violations(root, write)) == {"backends/toydb/e.cpp"}


def _sandbox_backend(root: Path, policy: str, monkeypatch: pytest.MonkeyPatch):
    from accel_pipeline.agent import StageRunRequest, StageRunResult, CodexExecBackend

    cfg = _cfg(root, write_policy=policy, repo_root=root)
    backend = CodexExecBackend(pipeline_config=cfg, model_config=SimpleNamespace(), query=6, stage="implementation")

    def fake(self, request):
        (root / "accelerators/toybench/q3/k.cpp").write_text("peer edit\n")
        return StageRunResult(backend="codex_exec", stage=request.stage, status="finished")

    monkeypatch.setattr(CodexExecBackend, "_run_codex", fake)
    monkeypatch.setattr(CodexExecBackend, "_log_violations", lambda self, policy, bad: None)
    return backend, StageRunRequest(stage="implementation", instructions="", user_message="", agent_name="a",
                                    model_id="m")


@pytest.mark.parametrize("policy,status", [("enforce", "error"), ("warn", "finished"), ("off", "finished")])
def test_write_policy_enforce_warn_off(tmp_path: Path, monkeypatch: pytest.MonkeyPatch, policy: str, status: str) -> None:
    root = _git_tree(tmp_path / "t")
    backend, request = _sandbox_backend(root, policy, monkeypatch)
    result = backend.run_stage(request)
    assert result.status == status
    if policy == "enforce":
        assert "accelerators/toybench/q3/k.cpp" in result.error and "cross-query" in result.error
        assert "put back" in result.error
    assert bool(result.metadata.get("write_violations")) == (policy != "off")
    # enforce reverts the out-of-bounds edit; warn and off leave it
    assert ((root / "accelerators/toybench/q3/k.cpp").read_text() == "x\n") == (policy == "enforce")


def test_a_revert_restores_each_path_as_the_stage_found_it(tmp_path: Path) -> None:
    root = _git_tree(tmp_path / "t")
    (root / "backends/toydb/e.cpp").write_text("dirty before the stage\n")  # an earlier stage's change stays
    check = StageWriteCheck(root, query=6, stage="implementation", pipeline_config=_cfg(root))
    (root / "backends/toydb/e.cpp").write_text("this stage's edit\n")
    (root / "substrate/s.h").write_text("this stage's edit\n")
    (root / "eval").mkdir()
    (root / "eval/moved.cpp").write_text("relocated code\n")
    (root / "accelerators/toybench/q6/k.cpp").write_text("mine\n")
    bad = check.violations()
    assert {rel for rel, _ in bad} == {"backends/toydb/e.cpp", "substrate/s.h", "eval/moved.cpp"}
    check.revert(bad)
    assert (root / "backends/toydb/e.cpp").read_text() == "dirty before the stage\n"
    assert (root / "substrate/s.h").read_text() == "x\n" and not (root / "eval/moved.cpp").exists()
    assert (root / "accelerators/toybench/q6/k.cpp").read_text() == "mine\n" and check.violations() == []


def test_no_check_runs_while_the_session_has_out_of_bounds_edits(tmp_path: Path) -> None:
    import json

    from accel_pipeline.mcp import ToolContext, admission_refusal_payload

    root = _git_tree(tmp_path / "t")
    cfg = _cfg(root, quick_tier=SimpleNamespace(enabled=False))
    ctx = ToolContext(cfg, 6, workload={}, stage="host_bootstrap",
                      writes=StageWriteCheck(root, query=6, stage="host_bootstrap", pipeline_config=cfg))
    assert admission_refusal_payload(ctx, "quick_check") == ""
    (root / "eval").mkdir()
    (root / "eval/sweep_session.cpp").write_text("moved out of the scanned harness dir\n")
    refusal = json.loads(admission_refusal_payload(ctx, "host_bootstrap"))
    assert refusal["status"] == "refused" and "eval/sweep_session.cpp" in refusal["reason"]


def test_engine_state_and_build_dirs_in_the_tree_are_not_agent_writes(tmp_path: Path, monkeypatch) -> None:
    root = _git_tree(tmp_path / "t")
    monkeypatch.setenv("ACCEL_ENGINE_STATE_DIR", str(root / ".engine_state"))

    def write(r: Path) -> None:
        (r / ".engine_state/build_stamps").mkdir(parents=True)
        (r / ".engine_state/build_stamps/abc").write_text("stamp")

    assert _violations(root, write) == {}


def test_dataset_inside_a_writable_bind_is_rebound_read_only(tmp_path: Path, isolated: Path) -> None:
    root = _tree(tmp_path / "tree").resolve()
    (root / "data").mkdir()
    dataset = root / "data" / "toybench.toydb"
    dataset.touch()
    binds = agent.cpp_mounts(_cfg(root, datasets={"toybench_sf1": dataset}))
    order = [(m, d) for m, _, d in binds]
    assert ("ro", dataset) in order and order.index(("rw", root)) < order.index(("ro", dataset))
