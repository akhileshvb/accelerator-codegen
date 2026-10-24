import json
import time
from types import SimpleNamespace

from accel_pipeline.backends.base import StageRunRequest, StageRunResult
from accel_pipeline.config import SessionConfig
from accel_pipeline.session import (
    HEARTBEAT_FILE, MAIN, SessionStore, budget_footer, build_handoff, context_tokens, keep_alive, run_in_session,
)


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


def test_build_handoff_without_any_record(tmp_path):
    assert "Handoff" in build_handoff(tmp_path)


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


def test_budget_footer_without_an_art_dir_is_empty(monkeypatch):
    monkeypatch.delenv("ACCEL_PIPELINE_ART_DIR", raising=False)
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
    time.sleep(0.2)
    assert beat.stat().st_mtime == last


def test_keep_alive_without_an_art_dir_is_a_no_op():
    with keep_alive(None):
        pass
