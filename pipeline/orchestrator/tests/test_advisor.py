"""The advisor: a stronger model forks the stuck agent's session for one opinion."""

import json
import subprocess
import threading
from pathlib import Path

import pytest

from accel_pipeline import advisor
from accel_pipeline import repair as repair_mod
from accel_pipeline.backends.base import StageRunResult
from accel_pipeline.backends.codex_exec import CodexExecBackend
from accel_pipeline.config import AdvisorConfig, ModelConfig, PipelineConfig, RepairConfig
from accel_pipeline.failure import Failure
from accel_pipeline.repair import RepairCtx, run_with_repair
from accel_pipeline.session import MAIN, SessionStore

FAKE_CODEX = Path(__file__).parent / "fixtures" / "fake_codex.sh"
REPO_ROOT = Path(__file__).resolve().parents[3]
SID = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"


def git(root, *args):
    subprocess.run(["git", "-C", str(root), "-c", "user.email=a@b", "-c", "user.name=t", *args], check=True,
                   capture_output=True)


class FakeFork:
    """Stands in for the advisor's backend: records the request and edits files like a misbehaving advisor."""

    def __init__(self, root, advice="Root cause: X.\nNext: Y.", status="finished"):
        self.root, self.advice, self.status, self.requests = root, advice, status, []

    def fork_stage(self, request):
        self.requests.append(request)
        (self.root / "accelerators/q3.cpp").write_text("advisor edit")  # tracked, modified
        (self.root / "accelerators/new.cpp").write_text("advisor file")  # untracked
        (self.root / "accelerators/dirty.cpp").write_text("advisor rewrote the agent's work")
        return StageRunResult(backend="codex_exec", stage=request.stage, status=self.status, final_text=self.advice)


class World:
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
            tpch_db_sf1=tmp_path, tpch_db_sf10=tmp_path, codex_home=tmp_path / "home", sandbox="none",
            write_policy="off", repair=RepairConfig(), advisor=AdvisorConfig(**advisor_cfg),
        )
        self.models = ModelConfig(api_key_env="K", presets={}, stages={}, codex_bin=str(FAKE_CODEX))
        self.art = tmp_path / "pipeline/artifacts/toybench/q3"
        self.art.mkdir(parents=True)
        self.fork = FakeFork(tmp_path)
        workload = {"benchmark_name": "toybench"}
        monkeypatch.setattr("accel_pipeline.workload.load_run_workload", lambda cfg, name: workload)
        monkeypatch.setattr("accel_pipeline.config.make_model_id", lambda *a, **k: "advisor/model")
        monkeypatch.setattr("accel_pipeline.prompts.render_prompt", lambda *a, **k: "ADVISOR INSTRUCTIONS")
        monkeypatch.setattr("accel_pipeline.backends.create_agent_backend", lambda **k: self.backend(**k))
        self.created = []

    def backend(self, **kwargs):
        self.created.append(kwargs)
        return self.fork

    def consult(self, stage="implementation", question="why?", trigger="agent", **kw):
        return advisor.consult(self.config, self.models, "w", 3, stage, question=question, trigger=trigger, **kw)

    def main_session(self, sid=SID):
        SessionStore(self.art).record(3, MAIN, sid, "implementation", resumed=False)

    def events(self):
        path = next((self.root / "pipeline/artifacts").rglob("pipeline_events.jsonl"))
        return [json.loads(line) for line in path.read_text().splitlines()]


@pytest.fixture
def world(tmp_path, monkeypatch):
    return World(tmp_path, monkeypatch)


# ---- the fork command ----

def fork_backend(tmp_path, monkeypatch, art=True):
    monkeypatch.setenv("FAKE_CODEX_MODE", "ok")
    monkeypatch.setenv("FAKE_CODEX_DIR", str(tmp_path))
    config = PipelineConfig(repo_root=tmp_path, pipeline_root=tmp_path / "pipeline", build_dir=tmp_path / "build",
                            tpch_db_sf1=tmp_path, tpch_db_sf10=tmp_path, codex_home=tmp_path / "home",
                            sandbox="none", write_policy="enforce")
    models = ModelConfig(api_key_env="K", presets={}, stages={}, codex_bin=str(FAKE_CODEX))
    return CodexExecBackend(pipeline_config=config, model_config=models, query=3, stage="implementation",
                            art_dir=tmp_path / "art" if art else None)


def test_the_fork_command_forks_the_session_on_the_advisor_model(tmp_path, monkeypatch):
    from accel_pipeline.backends.base import StageRunRequest

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
    from accel_pipeline.backends.base import StageRunRequest

    (tmp_path / "art").mkdir()
    (tmp_path / "art" / "agent_run.json").write_text("{}")
    backend.fork_stage(StageRunRequest(stage="advisor", instructions="I", user_message="q", agent_name="a",
                                       model_id="m", metadata={"fork_session_id": SID}))
    assert (tmp_path / "art" / "agent_run.json").exists()


# ---- consult ----

def test_consult_forks_the_main_session_and_undoes_the_advisors_edits(world):
    world.main_session()
    advice = world.consult()
    assert advice.startswith("Root cause: X.")
    (request,) = world.fork.requests
    assert request.metadata == {"fork_session_id": SID} and request.stage == "advisor"
    assert request.user_message == "why?" and request.instructions == "ADVISOR INSTRUCTIONS"
    assert world.created[0]["stage"] == "implementation"  # the agent's MCP setup
    root = world.root / "accelerators"
    assert (root / "q3.cpp").read_text() == "committed"
    assert not (root / "new.cpp").exists()
    assert (root / "dirty.cpp").read_text() == "the agent's work in progress"


def test_a_live_session_id_serves_a_first_turn_without_a_stored_one(world):
    world.consult(session_id="live-id")
    assert world.fork.requests[0].metadata["fork_session_id"] == "live-id"


def test_no_session_no_consult_and_no_count(world):
    with pytest.raises(advisor.Unavailable, match="no session"):
        world.consult()
    assert advisor.consults_used(world.art, "implementation") == 0 and not world.fork.requests


def test_consults_are_capped_per_stage_family(tmp_path, monkeypatch):
    world = World(tmp_path, monkeypatch, max_consults_per_stage=2)
    world.main_session()
    world.consult()
    world.consult(stage="impl_host")  # same family
    with pytest.raises(advisor.Unavailable, match="limit reached"):
        world.consult()
    world.consult(stage="optimization_unified")  # its own cap
    assert len(world.fork.requests) == 3


def test_a_disabled_advisor_is_unavailable(tmp_path, monkeypatch):
    world = World(tmp_path, monkeypatch, max_consults_per_stage=0)
    world.main_session()
    with pytest.raises(advisor.Unavailable, match="disabled"):
        world.consult()


def test_a_consult_is_logged_as_an_event_and_in_the_experiment_log(world):
    world.main_session()
    world.consult(trigger="stall")
    (event,) = [e for e in world.events() if e["event"] == "advisor_consult"]
    assert event["trigger"] == "stall" and event["stage"] == "implementation" and event["consult"] == 1
    assert event["session_id"] == SID and event["model"] == "advisor/model" and event["edits_undone"] == 3
    log = (world.art / "experiment_log.md").read_text()
    assert "advisor[implementation] consult 1/4 (stall" in log


def test_an_advisor_that_fails_gives_no_advice(world):
    world.main_session()
    world.fork.status, world.fork.advice = "error", ""
    with pytest.raises(advisor.Unavailable, match="no answer"):
        world.consult()
    assert (world.root / "accelerators/dirty.cpp").read_text() == "the agent's work in progress"


# ---- the agent's tool ----

def serve(world, stage="implementation"):
    return advisor.Servicer(world.config, world.models, "w", 3, stage, 0, world.art)


def test_ask_gets_the_servicers_answer(world):
    world.main_session()
    servicer = serve(world)
    answers = []
    asker = threading.Thread(target=lambda: answers.append(advisor.ask(world.art, "stuck on X", 20, poll_sec=0.05)))
    asker.start()
    while asker.is_alive():
        servicer.poll()
        asker.join(0.05)
    servicer.join()
    assert answers[0].startswith("Root cause: X.")
    assert world.fork.requests[0].user_message == "stuck on X"
    assert not (world.art / advisor.REQUEST_FILE).exists()


def test_ask_reports_a_refusal(world):
    servicer = serve(world)  # no session yet
    asker = threading.Thread(target=lambda: world.__dict__.update(answer=advisor.ask(world.art, "q", 20, poll_sec=0.05)))
    asker.start()
    while asker.is_alive():
        servicer.poll()
        asker.join(0.05)
    assert world.answer.startswith("No advice: this stage has no session")


def test_ask_gives_up_when_nobody_serves(world):
    assert "did not answer in time" in advisor.ask(world.art, "q", 0.2, poll_sec=0.05)
    assert not (world.art / advisor.REQUEST_FILE).exists()


def mcp_tools(world, stage="implementation"):
    from accel_pipeline.mcp_server import build_mcp_tool_callables

    return build_mcp_tool_callables(world.config, {"benchmark_name": "toybench"}, "w", 3, stage, 0, world.art)


def test_the_mcp_tool_counts_down_and_stops_at_the_cap(tmp_path, monkeypatch):
    world = World(tmp_path, monkeypatch, max_consults_per_stage=1)
    monkeypatch.setattr(advisor, "ask", lambda art, question, timeout: "advice")
    ask = mcp_tools(world)["ask_advisor"]
    assert ask("q").endswith("[advisor consults left in this stage: 0]")
    world.main_session()
    world.consult()  # the servicer's side spends the one consult
    assert "limit reached" in ask("q")


def test_the_mcp_tool_is_for_implementation_optimization_and_their_repair_turns(world, monkeypatch):
    monkeypatch.setattr(advisor, "ask", lambda art, question, timeout: "advice")
    assert "not available" in mcp_tools(world, stage="analysis")["ask_advisor"]("q")
    assert mcp_tools(world, stage="crash_debug")["ask_advisor"]("q").startswith("advice")


# ---- the repair loop ----

class Loop:
    def __init__(self, world, monkeypatch, texts):
        self.world, self.messages, self.consults = world, [], []
        self.texts = list(texts)
        monkeypatch.setattr(repair_mod, "load_run_workload", lambda cfg, name: {"benchmark_name": "toybench"})
        monkeypatch.setattr(repair_mod, "_run_gates", self._gates)
        monkeypatch.setattr(repair_mod, "_rollback", lambda ctx: None)
        monkeypatch.setattr(repair_mod, "_replan", lambda ctx, failure: False)
        monkeypatch.setattr(advisor, "consult", self._consult)

    def _gates(self, ctx, names):
        from accel_pipeline.gates.run import GateResult

        if not self.texts:
            return [GateResult("g", "pass", {})], None
        result = GateResult("g", "fail", {"log_tail": self.texts.pop(0), "returncode": 1})
        return [result], result

    def _consult(self, *args, **kw):
        self.consults.append(kw)
        return "use the delta path"

    def run(self, **kw):
        ctx = RepairCtx(self.world.config, None, "w", 3, stage="implementation", change_kind="implementation",
                        repair_turn=lambda failure: self.messages.append(repair_mod.render_failure(failure)), **kw)
        run_with_repair(ctx, lambda: None, gates=["g"])


def test_the_same_failure_twice_brings_the_advisor_into_the_next_message(world, monkeypatch):
    loop = Loop(world, monkeypatch, ["quick_check failed: rebuild on write", "quick_check failed: rebuild on write"])
    loop.run()
    assert len(loop.consults) == 1 and loop.consults[0]["trigger"] == "stall"
    assert "A reviewer with your full context diagnosed:\nuse the delta path" not in loop.messages[0]
    assert "A reviewer with your full context diagnosed:\nuse the delta path" in loop.messages[1]


def test_a_changing_failure_is_not_a_stall(world, monkeypatch):
    loop = Loop(world, monkeypatch, ["error one", "error two", "error three"])
    loop.run()
    assert loop.consults == []


def test_digits_do_not_make_a_failure_new(world, monkeypatch):
    loop = Loop(world, monkeypatch, ["failed at 12.5s, 3 of 9", "failed at 13.1s, 4 of 9"])
    loop.run()
    assert len(loop.consults) == 1


def test_one_consult_per_stall(world, monkeypatch):
    loop = Loop(world, monkeypatch, ["same"] * 3)
    loop.run()
    assert len(loop.consults) == 1


def test_a_no_progress_redirect_consults_at_once(world, monkeypatch):
    world.config.repair = RepairConfig()
    loop = Loop(world, monkeypatch, [])
    failure = Failure(kind="timeout", stage="implementation", source="no_progress", summary="no progress for 1200s")
    attempts = iter([failure])

    def attempt():
        from accel_pipeline.failure import StageFailure

        for f in attempts:
            raise StageFailure(f)

    ctx = RepairCtx(world.config, None, "w", 3, stage="implementation", change_kind="implementation",
                    repair_turn=lambda f: loop.messages.append(repair_mod.render_failure(f)))
    run_with_repair(ctx, attempt, gates=["g"])
    assert len(loop.consults) == 1 and "A reviewer with your full context" in loop.messages[0]


def test_an_unavailable_advisor_does_not_stop_the_repair(world, monkeypatch):
    loop = Loop(world, monkeypatch, ["same", "same"])

    def refuse(*a, **kw):
        raise advisor.Unavailable("advisor limit reached (4 consults in implementation)")

    monkeypatch.setattr(advisor, "consult", refuse)
    loop.run()
    assert len(loop.messages) == 2 and all("A reviewer" not in m for m in loop.messages)


# ---- configuration ----
