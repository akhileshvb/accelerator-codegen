"""A second opinion for a stuck agent: a stronger model forks the agent's own codex session (full context)
and answers once. The fork is a new session; the agent's is never touched, and files the advisor changes
are undone. Consults are capped per stage and logged.

Two ways in: the agent's `ask_advisor` tool (a request file the codex supervisor serves while the agent
waits in the tool call), and the repair loop (a stalled failure or a no-progress redirect)."""

from __future__ import annotations

import shutil
import subprocess
import tempfile
import threading
import time
import uuid
from pathlib import Path
from typing import Any

from accel_pipeline import session as session_mod
from accel_pipeline.session import MAIN, SessionStore

STAGE = "advisor"
REQUEST_FILE = "advisor_request.json"
RESPONSE_FILE = "advisor_response.json"
COUNT_FILE = "advisor_consults.json"
READ_ONLY_TOOLS = ["quick_check", "gate_status", "get_profile_digest", "get_store_diagnostics",
                   "get_optimization_evidence", "discover_host_sdk"]
MAX_ADVICE_CHARS = 6000
ADVICE_INTRO = "A reviewer with your full context diagnosed:"


class Unavailable(Exception):
    """No consult happens: the cap is spent, the stage has no session to fork, or the advisor is off."""


ADVISED = frozenset({"implementation", "optimization", "crash_debug"})  # crash_debug: the repair turns of either


def scope(stage: str) -> str:
    """The stage family that shares one consult cap."""
    return "optimization" if stage.startswith("optimization") else "implementation" if stage.startswith("impl") else stage


def consults_used(art: Path, stage: str) -> int:
    return int(session_mod._read(art / COUNT_FILE).get(scope(stage), 0))


def _claim(art: Path, stage: str, cap: int) -> int:
    used = consults_used(art, stage)
    if used >= cap:
        raise Unavailable(f"advisor limit reached ({cap} consults in {scope(stage)})")
    session_mod._write(art / COUNT_FILE, {**session_mod._read(art / COUNT_FILE), scope(stage): used + 1})
    return used + 1


def snapshot_tree(repo_root: Path, dest: Path) -> dict[str, str]:
    """The changed files (git status) and their signatures, copied into `dest`."""
    from accel_pipeline.layout import tree_snapshot

    before = tree_snapshot(repo_root) or {}
    for rel in before:
        src = repo_root / rel
        if src.is_file() and not src.is_symlink():
            (dest / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dest / rel)
    return before


def restore_tree(repo_root: Path, before: dict[str, str], saved: Path) -> list[str]:
    """Undo every change since `snapshot_tree`; returns the paths put back."""
    from accel_pipeline.layout import tree_snapshot

    now = tree_snapshot(repo_root) or {}
    undone = []
    for rel in sorted(before.keys() | now.keys()):
        if before.get(rel) == now.get(rel):
            continue
        target = repo_root / rel
        if (saved / rel).is_file():
            shutil.copy2(saved / rel, target)
        elif rel not in before and subprocess.run(["git", "-C", str(repo_root), "checkout", "HEAD", "--", rel],
                                                  capture_output=True).returncode == 0:
            pass  # a tracked file the advisor edited: back at HEAD
        else:
            target.unlink(missing_ok=True)
        undone.append(rel)
    return undone


def consult(pipeline_config: Any, model_config: Any, workload_name: str, query: int, stage: str, *,
            question: str, trigger: str, optimization_iter: int = 0, session_id: str = "") -> str:
    """Fork the stage's main session on the advisor model and return its advice (raises Unavailable)."""
    from accel_pipeline.backends import StageRunRequest, create_agent_backend
    from accel_pipeline.workload import load_run_workload, query_artifacts_dir
    from accel_pipeline.config import make_model_id
    from accel_pipeline.pipeline_log import log_event
    from accel_pipeline.playbook import append_experiment_log
    from accel_pipeline.prompts import render_prompt

    cfg = pipeline_config.advisor
    if cfg.max_consults_per_stage <= 0:
        raise Unavailable("the advisor is disabled")
    repo_root = pipeline_config.repo_root
    workload = load_run_workload(pipeline_config, workload_name)
    art = query_artifacts_dir(repo_root, query, workload=workload)
    sid = SessionStore(art).session_id(query, MAIN) or session_id
    if not sid:
        raise Unavailable("this stage has no session to fork yet")
    number = _claim(art, stage, cfg.max_consults_per_stage)

    model_id = make_model_id(STAGE, pipeline_config, model_config)
    backend = create_agent_backend(pipeline_config=pipeline_config, query=query, stage=stage, model_config=model_config,
                                   workload_name=workload_name, optimization_iter=optimization_iter)
    backend.timeout_sec = cfg.timeout_sec
    request = StageRunRequest(
        stage=STAGE, instructions=render_prompt(pipeline_config, "05_advisor.md", workload, query, stage, optimization_iter),
        user_message=question, agent_name=f"accel_{STAGE}", model_id=model_id, agent_model=model_id,
        metadata={"fork_session_id": sid})
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="accel_advisor_") as tmp:
        before = snapshot_tree(repo_root, Path(tmp))
        try:
            result = backend.fork_stage(request)
        finally:
            undone = restore_tree(repo_root, before, Path(tmp))
    advice = (result.final_text or "").strip()[-MAX_ADVICE_CHARS:] if result.status == "finished" else ""
    log_event(repo_root, query, workload, "advisor_consult", stage=stage, trigger=trigger, model=model_id,
              session_id=sid, consult=number, status=result.status, advice_chars=len(advice), edits_undone=len(undone),
              duration_ms=int((time.monotonic() - started) * 1000))
    append_experiment_log(
        repo_root, query, f"- advisor[{stage}] consult {number}/{cfg.max_consults_per_stage} ({trigger}, {model_id}): "
        f"{result.status}, {len(advice)} chars" + (f", undid edits to {', '.join(undone[:5])}" if undone else ""),
        workload=workload)
    if not advice:
        raise Unavailable(f"the advisor gave no answer ({result.status}: {(result.error or '')[:200]})")
    return advice


def advise_message(advice: str) -> str:
    return f"{ADVICE_INTRO}\n{advice}"


# ---- agent-initiated: request/response files between the MCP tool and the codex supervisor ----

def ask(art: Path, question: str, timeout_sec: float, poll_sec: float = 2.0) -> str:
    """The `ask_advisor` tool: file a request and wait for the supervisor's answer."""
    request_id = uuid.uuid4().hex
    (art / RESPONSE_FILE).unlink(missing_ok=True)
    session_mod._write(art / REQUEST_FILE, {"id": request_id, "question": question})
    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        response = session_mod._read(art / RESPONSE_FILE)
        if response.get("id") == request_id:
            return str(response.get("advice") or f"No advice: {response.get('reason', 'unavailable')}.")
        time.sleep(poll_sec)
    (art / REQUEST_FILE).unlink(missing_ok=True)
    return "No advice: the advisor did not answer in time. Carry on with your own diagnosis."


class Servicer:
    """Run by the codex supervisor during an agent turn: serves ask_advisor requests, one at a time."""

    def __init__(self, pipeline_config: Any, model_config: Any, workload_name: str, query: int, stage: str,
                 optimization_iter: int, art: Path) -> None:
        self._args = (pipeline_config, model_config, workload_name, query, stage)
        self._iter, self._art = optimization_iter, art
        self._thread: threading.Thread | None = None
        self._served = ""
        (art / REQUEST_FILE).unlink(missing_ok=True)  # a request left by an earlier turn is nobody's

    def poll(self, live_session_id: str = "") -> None:
        request = session_mod._read(self._art / REQUEST_FILE)
        if not request or request.get("id") == self._served or (self._thread and self._thread.is_alive()):
            return
        self._served = str(request.get("id"))
        self._thread = threading.Thread(target=self._answer, args=(request, live_session_id), daemon=True)
        self._thread.start()

    def join(self) -> None:
        if self._thread:
            self._thread.join()

    def _answer(self, request: dict[str, Any], live_session_id: str) -> None:
        response: dict[str, Any] = {"id": request.get("id")}
        try:
            response["advice"] = consult(*self._args, question=str(request.get("question") or ""), trigger="agent",
                                         optimization_iter=self._iter, session_id=live_session_id)
        except Unavailable as exc:
            response["reason"] = str(exc)
        except Exception as exc:  # noqa: BLE001 - the agent's turn goes on without advice
            response["reason"] = f"{type(exc).__name__}: {exc}"
        session_mod._write(self._art / RESPONSE_FILE, response)
        (self._art / REQUEST_FILE).unlink(missing_ok=True)
