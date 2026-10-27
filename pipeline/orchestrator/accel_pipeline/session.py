"""One agent turn: session continuity, wall budgets and the no-progress watchdog.

A role keeps one codex session across rounds and phases (resumed, or replaced with a handoff once retired). While a
turn runs, the supervisor keeps a `ProgressClock`: reads, greps and file views are activity; progress is a change
under the stage's owned paths, a stage artifact written, or a check (quick_check, a gate) whose result differs from
its previous one; in the analysis stages also a successful data-tool call (data_queries.jsonl), since measuring the
data is their work. The MCP tool footer tells the agent its wall budget and how long it has gone without progress,
and past `redirect_sec` the turn is ended with a directed resume. `report_blocker` is the agent's way out when the
obstacle is not in its files.
"""

from __future__ import annotations

import fnmatch
import json
import os
import re
import subprocess
import threading
import time
from contextlib import contextmanager, suppress
from dataclasses import replace
from datetime import datetime
from pathlib import Path
from typing import Any, Callable, Iterator

from accel_pipeline.layout import frozen_edited, layer_write_verdict, tree_snapshot

SESSIONS_FILE = "sessions.json"
META_FILE = "session_meta.json"
RUN_FILE = "agent_run.json"
HEARTBEAT_FILE = "agent_heartbeat"
PROGRESS_FILE = "agent_progress.json"  # the supervisor's progress clock

MAIN = "main"
READONLY = "readonly"
TESTER = "tester"  # the test_suite stage: its own session, never the implementer's

CHECKS_FILE = "check_signatures.json"  # written by the MCP server: {check: signature of its last result}
BLOCKER_FILE = "blocker.md"
DATA_LOG = "data_queries.jsonl"  # tools/data_tools.py

CHECK_TOOLS = frozenset({"quick_check", "quick_layer", "request_gate", "run_engine_build", "run_layer_gate"})
STAGE_ARTIFACTS = ("analysis*.json", "experiment_log.md", "layer_critique_*", "test_suite_change_request*")
DATA_STAGES = frozenset({"analysis", "analysis_critic"})
GUARDED_READ_LIMIT = 10
_GUARDED_READ = re.compile(r"(?:^|[\s'\"=:(/])(?:pipeline/(?:orchestrator|gates|clean_tree)|scripts)/")
_SHELL_TOOLS = frozenset({"exec_command", "shell", "local_shell"})
_VOLATILE = re.compile(r"0x[0-9a-fA-F]+|\d+")
_BUSY_BEAT_SEC = 45.0  # a heartbeat this fresh means an MCP tool call is running (keep_alive)

NUDGE = ("No progress for {minutes} min. In one short paragraph in the experiment log: what you're trying, "
         "what blocks you, the next concrete step. If the block is outside what you own (the pipeline, a "
         "gate, the harness, the backend), call report_blocker now instead of investigating it.")
GUARDED_READS_NOTE = ("{count} shell reads of pipeline internals (orchestrator, gates, clean_tree, scripts) this "
                      "turn. A failure you cannot fix from your files is not yours to work around: call "
                      "report_blocker with the evidence.")
REDIRECT = ("You made no progress and your turn was ended. Pick one: fix the failure in your "
            "own files, call report_blocker (the cause is outside your files), or call "
            "request_structural_reanalysis (the design cannot work).")


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
    """The codex rollout log of a session (the newest, if codex wrote several)."""
    if not session_id:
        return None
    found = list((Path(codex_home) / "sessions").rglob(f"rollout-*{session_id}.jsonl"))
    return max(found, key=lambda p: p.stat().st_mtime) if found else None


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
    if cfg.max_resumes and store.resumes(query, role) >= cfg.max_resumes:
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
                            "soft_deadline_sec": cfg.soft_deadline_for(stage), "hard_deadline_sec": cfg.hard_deadline_sec,
                            "no_progress": dict(zip(("nudge_sec", "redirect_sec"), cfg.no_progress.for_stage(stage)))})
    try:
        yield
    finally:
        for name in (RUN_FILE, PROGRESS_FILE):
            (art / name).unlink(missing_ok=True)


def clear_stale_run(art: Path) -> None:
    """A run killed mid-stage leaves its agent markers (agent_run's cleanup never ran); none is live at startup."""
    for name in (RUN_FILE, PROGRESS_FILE):
        (art / name).unlink(missing_ok=True)


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
        _age_heartbeat(art)  # the call is over: a fresh beat must not make the next minute look busy


def _age_heartbeat(art: Path) -> None:
    beat = art / HEARTBEAT_FILE
    stale = time.time() - 120
    with suppress(OSError):
        os.utime(beat, (stale, stale))


def budget_footer(pipeline_config: Any, stage: str, query: int) -> str:
    """Text appended to every MCP tool result: empty, or a wrap-up/checkpoint or no-progress notice."""
    art = os.environ.get("ACCEL_PIPELINE_ART_DIR")
    run = _read(Path(art) / RUN_FILE) if art else {}
    if not run:
        return ""
    return "\n".join(filter(None, [_wall_notice(run), footer(Path(art)), _frozen_edit_notice(pipeline_config)]))


def _frozen_edit_notice(pipeline_config: Any) -> str:
    repo_root = getattr(pipeline_config, "repo_root", None)
    paths = frozen_edited(Path(repo_root)) if repo_root else []
    if not paths:
        return ""
    shown = ", ".join(paths[:5]) + (f" (+{len(paths) - 5} more)" if len(paths) > 5 else "")
    return (f"[pipeline files are not yours: {shown} edited. They are reverted when this stage ends, so the edit "
            "does nothing. Undo it now; if the task needs a change there, call report_blocker.]")


def _wall_notice(run: dict[str, Any]) -> str:
    elapsed = time.time() - float(run.get("started", time.time()))
    soft, hard = float(run.get("soft_deadline_sec") or 0), float(run.get("hard_deadline_sec") or 0)
    if hard and elapsed >= hard - min(600.0, hard / 10):
        return (f"[wall budget: {elapsed / 60:.0f} of {hard / 60:.0f} min used. This run is about to be stopped: "
                "checkpoint now (leave the tree building, note status in the experiment log) and finish.]")
    if soft and elapsed >= soft:
        return (f"[wall budget: {elapsed / 60:.0f} min used (soft limit {soft / 60:.0f}). Wrap up: "
                "finish or revert the current change, checkpoint the state of the tree, and end your turn.]")
    return ""


def check_signature(result: str) -> str:
    """What identifies a check result: its status and failure, without timings, counts or addresses."""
    try:
        data = json.loads(result)
    except ValueError:
        data = None
    if isinstance(data, dict):
        status = str(data.get("status", ""))
        detail = next((str(data[k]) for k in ("failure_summary", "failure", "summary", "notes") if data.get(k)), "")
    else:
        status, detail = "", result
    return f"{status}:{_VOLATILE.sub('#', detail.strip())[:300]}"


def record_check(art: Path, tool: str, args: tuple, result: str) -> None:
    """Remember the signature of a check tool's result; a different one is progress."""
    if tool not in CHECK_TOOLS:
        return
    key = f"{tool}:{args[0]}" if tool == "request_gate" and args else tool
    path = art / CHECKS_FILE
    try:
        known = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        known = {}
    signature = check_signature(result)
    if known.get(key) != signature:
        path.write_text(json.dumps({**known, key: signature}), encoding="utf-8")


def write_blocker(art: Path, summary: str, evidence: str) -> Path:
    path = art / BLOCKER_FILE
    path.write_text(f"# Blocker reported by the agent\n\n{summary.strip()}\n\n## Evidence\n\n{evidence.strip()}\n",
                    encoding="utf-8")
    return path


def footer(art: Path) -> str:
    """The no-progress notices for the MCP tool footer: empty while the agent is making progress."""
    progress = _read(art / PROGRESS_FILE)
    nudge = float((_read(art / RUN_FILE).get("no_progress") or {}).get("nudge_sec") or 0)
    if not progress:
        return ""
    lines = []
    idle = time.time() - float(progress.get("last_progress") or time.time())
    if nudge and idle >= nudge:
        lines.append(NUDGE.format(minutes=int(idle // 60)))
    reads = int(progress.get("guarded_reads") or 0)
    if reads > GUARDED_READ_LIMIT:
        lines.append(GUARDED_READS_NOTE.format(count=reads))
    return "[" + " ".join(lines) + "]" if lines else ""


class _ReadCounter:
    """Shell commands in the session log, since the turn started, that look at pipeline internals."""

    def __init__(self, since: float) -> None:
        self._since, self._offset, self.count = since, 0, 0

    def update(self, rollout: Path) -> None:
        with rollout.open("rb") as f:
            f.seek(self._offset)
            data = f.read()
        complete = data[: data.rfind(b"\n") + 1]
        self._offset += len(complete)
        for line in complete.decode(errors="replace").splitlines():
            if '"function_call"' in line and self._guarded(line):
                self.count += 1

    def _guarded(self, line: str) -> bool:
        try:
            record = json.loads(line)
            payload = record.get("payload") or {}
            when = datetime.fromisoformat(str(record.get("timestamp", "")).replace("Z", "+00:00")).timestamp()
            if payload.get("name") not in _SHELL_TOOLS or when < self._since:
                return False
            args = json.loads(payload.get("arguments") or "{}")
        except (ValueError, TypeError, AttributeError):
            return False
        cmd = args.get("cmd") or args.get("command") or ""
        return bool(_GUARDED_READ.search(" ".join(cmd) if isinstance(cmd, list) else str(cmd)))


class ProgressClock:
    """Samples what counts as progress during one agent turn; `poll()` says "", "nudge" or "redirect"."""

    def __init__(self, art: Path, repo_root: Path, *, query: int, stage: str, pipeline_config: Any,
                 nudge_sec: float, redirect_sec: float, started_wall: float, sample_sec: float = 15.0) -> None:
        self.art, self.repo_root = Path(art), Path(repo_root)
        self.query, self.stage, self.cfg = int(query), stage, pipeline_config
        self.nudge_sec, self.redirect_sec, self.sample_sec = nudge_sec, redirect_sec, sample_sec
        self._reads = _ReadCounter(started_wall)
        self._baseline = self._owned_state()
        self._fingerprint = self._sample()
        self._last_poll = self._last_sample = self.last_progress = time.monotonic()
        self._publish()

    def poll(self, rollout: Path | None = None) -> str:
        now = time.monotonic()
        busy = self._tool_running()
        if busy:  # time inside a tool call (a gate, a build) is not time spent without progress
            self.last_progress += now - self._last_poll
        self._last_poll = now
        if now - self._last_sample >= self.sample_sec:
            self._last_sample = now
            fingerprint = self._sample()
            if fingerprint != self._fingerprint:
                self._fingerprint, self.last_progress = fingerprint, now
            if rollout is not None and rollout.is_file():
                self._reads.update(rollout)
            self._publish()
        idle = now - self.last_progress
        if self.redirect_sec and idle >= self.redirect_sec:
            return "redirect"
        return "nudge" if self.nudge_sec and idle >= self.nudge_sec else ""

    def idle_sec(self) -> float:
        return time.monotonic() - self.last_progress

    def redirect_report(self) -> str:
        """The evidence of a directed resume: the last failure and what changed under the agent's files."""
        failure = self.art / "last_failure.md"
        parts = []
        if failure.is_file():
            parts += ["Last failure:", failure.read_text(encoding="utf-8")[-2000:].strip()]
        parts += ["Changed under your files since this turn started:", self._changes()]
        return "\n\n".join(parts)

    def _tool_running(self) -> bool:
        try:
            return time.time() - (self.art / HEARTBEAT_FILE).stat().st_mtime < _BUSY_BEAT_SEC
        except OSError:
            return False

    def _publish(self) -> None:
        _write(self.art / PROGRESS_FILE, {"last_progress": time.time() - self.idle_sec(),
                                          "guarded_reads": self._reads.count})

    def _owned_state(self) -> dict[str, str]:
        """{path: content signature} of the changed files this stage may write."""
        cfg = self.cfg
        identity = dict(workload_kind=str(getattr(cfg, "workload_kind", "") or ""),
                        engine_name=str(getattr(cfg, "engine_name", "") or ""))
        if getattr(cfg, "benchmark_name", ""):  # else the layout's own default applies
            identity["benchmark"] = str(cfg.benchmark_name)
        snapshot = tree_snapshot(self.repo_root) or {}
        return {rel: sig for rel, sig in snapshot.items()
                if (layer_write_verdict(rel, query=self.query, stage=self.stage, **identity) or (False,))[0]}

    def _artifacts(self) -> list[tuple[str, int, int]]:
        try:
            files = [p for p in self.art.iterdir() if p.is_file()]
        except OSError:
            return []
        return sorted((p.name, p.stat().st_mtime_ns, p.stat().st_size) for p in files
                      if any(fnmatch.fnmatch(p.name, g) for g in STAGE_ARTIFACTS))

    def _data_calls(self) -> int:
        """Successful data-tool calls so far (analysis stages only)."""
        if self.stage not in DATA_STAGES:
            return 0
        try:
            lines = (self.art / DATA_LOG).read_text(encoding="utf-8").splitlines()
        except OSError:
            return 0
        return sum('"error": null' in line for line in lines)

    def _sample(self) -> tuple:
        try:
            checks = (self.art / CHECKS_FILE).read_text(encoding="utf-8")
        except OSError:
            checks = ""
        return (sorted(self._owned_state().items()), self._artifacts(), checks, self._data_calls())

    def _changes(self) -> str:
        now = self._owned_state()
        changed = sorted(rel for rel, sig in now.items() if self._baseline.get(rel) != sig)
        if not changed:
            return "no file of yours changed."
        tracked = subprocess.run(["git", "-C", str(self.repo_root), "diff", "--stat", "HEAD", "--", *changed],
                                 capture_output=True, text=True).stdout.strip()
        new = [rel for rel in changed if rel not in tracked]
        return "\n".join(filter(None, [tracked, *(f"new or untracked: {rel}" for rel in new)]))
