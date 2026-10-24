"""Codex session continuity (one session per role, resumed across rounds and phases) and wall budgets."""

from __future__ import annotations

import json
import os
import threading
import time
from contextlib import contextmanager
from dataclasses import replace
from pathlib import Path
from typing import Any, Callable, Iterator

SESSIONS_FILE = "sessions.json"
META_FILE = "session_meta.json"
RUN_FILE = "agent_run.json"
HEARTBEAT_FILE = "agent_heartbeat"

MAIN = "main"
READONLY = "readonly"
TESTER = "tester"  # the test_suite stage: its own session, never the implementer's


def _read(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return data if isinstance(data, dict) else {}


def _write(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f"{path.name}.{os.getpid()}.{threading.get_ident()}.tmp")
    tmp.write_text(json.dumps(data, indent=2), encoding="utf-8")
    os.replace(tmp, path)


class SessionStore:
    """<art>/sessions.json is {query: {role: session_id}}; resume counts and the phases whose
    instructions the session has seen live beside it in session_meta.json."""

    def __init__(self, art: Path) -> None:
        self._ids = art / SESSIONS_FILE
        self._meta = art / META_FILE

    def session_id(self, query: int, role: str) -> str:
        return str(_read(self._ids).get(str(query), {}).get(role) or "")

    def resumes(self, query: int, role: str) -> int:
        return int(self._role_meta(query, role).get("resumes", 0))

    def phases(self, query: int, role: str) -> list[str]:
        return list(self._role_meta(query, role).get("phases", []))

    def record(self, query: int, role: str, session_id: str, phase: str, *, resumed: bool) -> None:
        ids = _read(self._ids)
        ids.setdefault(str(query), {})[role] = session_id
        _write(self._ids, ids)
        meta = _read(self._meta)
        entry = meta.setdefault(str(query), {}).setdefault(role, {})
        if resumed:
            entry["resumes"] = int(entry.get("resumes", 0)) + 1
        else:
            entry.update(resumes=0, phases=[])
        if phase not in entry.setdefault("phases", []):
            entry["phases"].append(phase)
        _write(self._meta, meta)

    def reset(self, query: int, role: str) -> None:
        for path in (self._ids, self._meta):
            data = _read(path)
            if data.get(str(query), {}).pop(role, None) is not None:
                _write(path, data)

    def _role_meta(self, query: int, role: str) -> dict[str, Any]:
        return _read(self._meta).get(str(query), {}).get(role, {})


def rollout_path(codex_home: Path, session_id: str) -> Path | None:
    from accel_pipeline.verus.usage import _rollout

    return _rollout(codex_home, session_id)


def context_tokens(codex_home: Path, session_id: str) -> int:
    """The context the session's last model call read (0 when unknown)."""
    path = rollout_path(codex_home, session_id)
    if path is None:
        return 0
    last = 0
    with path.open(encoding="utf-8") as f:
        for line in f:
            if '"token_count"' not in line:
                continue
            try:
                info = (json.loads(line).get("payload") or {}).get("info") or {}
            except json.JSONDecodeError:
                continue
            last = int((info.get("last_token_usage") or {}).get("input_tokens") or last)
    return last


def build_handoff(art: Path) -> str:
    """What a fresh session needs to carry on: the experiment log tail, the best snapshot, the last failure."""
    parts = ["## Handoff from the previous session", "",
             "Your earlier session was retired (cap or lost log). Its work is on disk; this is where it stood."]
    log = art / "experiment_log.md"
    if log.is_file():
        tail = "\n".join(log.read_text(encoding="utf-8").splitlines()[-40:])
        parts += ["", "### Experiment log (tail)", tail]
    manifest = _read(art / "best_kernel_snapshot" / "manifest.json")
    if manifest:
        parts += ["", "### Best snapshot",
                  f"{manifest.get('speedup_median', '?')}x from {manifest.get('change_kind', '?')} "
                  f"iter {manifest.get('iteration', '?')}"]
    failure = art / "last_failure.md"
    if failure.is_file():
        parts += ["", "### Last failure", failure.read_text(encoding="utf-8")[-3000:]]
    return "\n".join(parts) + "\n\n"


def _retired(pipeline_config: Any, store: SessionStore, query: int, role: str,
             codex_home: Path | None, session_id: str) -> bool:
    cfg = pipeline_config.session
    if store.resumes(query, role) >= cfg.max_resumes:
        return True
    if codex_home is None:
        return False
    if rollout_path(codex_home, session_id) is None:
        return True
    return bool(cfg.max_context_tokens and context_tokens(codex_home, session_id) >= cfg.max_context_tokens)


def run_in_session(backend: Any, request: Any, *, pipeline_config: Any, art: Path, query: int, role: str,
                   handoff: Callable[[], str] | None = None) -> Any:
    """Run a stage request in the role's session: resume it (the message only, plus the stage's
    instructions the first time the session sees that stage), or start fresh with a handoff
    (`handoff()` when given, else the generic one from the run's artifacts)."""
    if not pipeline_config.session.continuity:
        return backend.run_stage(request)
    store = SessionStore(art)
    home = getattr(backend, "codex_home", None)
    codex_home = home() if callable(home) else None
    sid = store.session_id(query, role)
    message = request.user_message
    if sid and _retired(pipeline_config, store, query, role, codex_home, sid):
        message, sid = (handoff() if handoff else build_handoff(art)) + message, ""
        store.reset(query, role)
    metadata = dict(request.metadata)
    if sid:
        metadata.update(resume_session_id=sid, phase_change=request.stage not in store.phases(query, role))
    result = backend.run_stage(replace(request, user_message=message, metadata=metadata))
    new_id = str(result.metadata.get("session_id") or "")
    if new_id:
        store.record(query, role, new_id, request.stage, resumed=new_id == sid)
    return result


@contextmanager
def agent_run(art: Path | None, stage: str, pipeline_config: Any) -> Iterator[None]:
    """Marks a running agent for the MCP budget footer: its start and deadlines."""
    if art is None:
        yield
        return
    cfg = pipeline_config.session
    _write(art / RUN_FILE, {"stage": stage, "started": time.time(),
                            "soft_deadline_sec": cfg.soft_deadline_for(stage), "hard_deadline_sec": cfg.hard_deadline_sec})
    try:
        yield
    finally:
        (art / RUN_FILE).unlink(missing_ok=True)


def heartbeat(art: Path) -> None:
    (art / HEARTBEAT_FILE).touch()


@contextmanager
def keep_alive(art: Path | None, every_sec: float = 30.0) -> Iterator[None]:
    """Counts as agent activity while a tool call blocks (a gate wait): beats until the block exits."""
    if art is None:
        yield
        return
    done = threading.Event()

    def beat() -> None:
        while not done.is_set():
            heartbeat(art)
            done.wait(every_sec)

    thread = threading.Thread(target=beat, daemon=True)
    thread.start()
    try:
        yield
    finally:
        done.set()
        thread.join(timeout=1)


def budget_footer(pipeline_config: Any, stage: str, query: int) -> str:
    """Text appended to every MCP tool result: empty, or a wrap-up/checkpoint notice near the deadline."""
    art = os.environ.get("ACCEL_PIPELINE_ART_DIR")
    run = _read(Path(art) / RUN_FILE) if art else {}
    if not run:
        return ""
    elapsed = time.time() - float(run.get("started", time.time()))
    soft, hard = float(run.get("soft_deadline_sec") or 0), float(run.get("hard_deadline_sec") or 0)
    if hard and elapsed >= hard - min(600.0, hard / 10):
        return (f"[wall budget: {elapsed / 60:.0f} of {hard / 60:.0f} min used. This run is about to be stopped: "
                "checkpoint now (leave the tree building, note status in the experiment log) and finish.]")
    if soft and elapsed >= soft:
        return (f"[wall budget: {elapsed / 60:.0f} min used (soft limit {soft / 60:.0f}). Wrap up: "
                "finish or revert the current change, checkpoint the state of the tree, and end your turn.]")
    return ""
