"""Progress of an agent turn, and what to do when there is none.

Reads, greps and file views are activity; progress is a change under the stage's owned paths, a stage
artifact written, or a check (quick_check, a gate) whose result differs from its previous one. The
supervisor of the codex process keeps a `ProgressClock`; the MCP tool footer tells the agent how long
it has gone without progress, and past `redirect_sec` the turn is ended with a directed resume.
`report_blocker` is the agent's way out when the obstacle is not in its files."""

from __future__ import annotations

import fnmatch
import json
import os
import re
import subprocess
import time
from datetime import datetime
from pathlib import Path
from typing import Any

CHECKS_FILE = "check_signatures.json"  # written by the MCP server: {check: signature of its last result}
BLOCKER_FILE = "blocker.md"

CHECK_TOOLS = frozenset({"quick_check", "quick_layer", "request_gate", "run_engine_build", "run_layer_gate"})
STAGE_ARTIFACTS = ("analysis*.json", "experiment_log.md", "layer_critique_*", "test_suite_change_request*")
GUARDED_READ_LIMIT = 10
_GUARDED_READ = re.compile(r"(?:^|[\s'\"=:(/])(?:pipeline/(?:orchestrator|gates|clean_tree)|scripts)/")
_SHELL_TOOLS = frozenset({"exec_command", "shell", "local_shell"})
_VOLATILE = re.compile(r"0x[0-9a-fA-F]+|\d+")
_BUSY_BEAT_SEC = 45.0  # a heartbeat this fresh means an MCP tool call is running (session.keep_alive)

NUDGE = ("No progress for {minutes} min. In one short paragraph in the experiment log: what you're trying, "
         "what blocks you, the next concrete step. If the block is outside what you own (the pipeline, a "
         "gate, the harness, the backend), call report_blocker now instead of investigating it.")
GUARDED_READS_NOTE = ("{count} shell reads of pipeline internals (orchestrator, gates, clean_tree, scripts) this "
                      "turn. A failure you cannot fix from your files is not yours to work around: call "
                      "report_blocker with the evidence.")
REDIRECT = ("You made no progress and your turn was ended. Pick one: fix the failure in your "
            "own files, call report_blocker (the cause is outside your files), or call "
            "request_structural_reanalysis (the design cannot work).")


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
    from accel_pipeline.session import PROGRESS_FILE, RUN_FILE, _read

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
        from accel_pipeline.session import HEARTBEAT_FILE

        try:
            return time.time() - (self.art / HEARTBEAT_FILE).stat().st_mtime < _BUSY_BEAT_SEC
        except OSError:
            return False

    def _publish(self) -> None:
        from accel_pipeline.session import PROGRESS_FILE, _write

        _write(self.art / PROGRESS_FILE, {"last_progress": time.time() - self.idle_sec(),
                                          "guarded_reads": self._reads.count})

    def _owned_state(self) -> dict[str, str]:
        """{path: content signature} of the changed files this stage may write."""
        from accel_pipeline.write_ownership import layer_write_verdict, tree_snapshot

        cfg = self.cfg
        identity = dict(workload_kind=str(getattr(cfg, "workload_kind", "") or ""),
                        engine_name=str(getattr(cfg, "engine_name", "") or ""))
        if getattr(cfg, "benchmark_name", ""):  # else write_ownership's own default applies
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

    def _sample(self) -> tuple:
        try:
            checks = (self.art / CHECKS_FILE).read_text(encoding="utf-8")
        except OSError:
            checks = ""
        return (sorted(self._owned_state().items()), self._artifacts(), checks)

    def _changes(self) -> str:
        now = self._owned_state()
        changed = sorted(rel for rel, sig in now.items() if self._baseline.get(rel) != sig)
        if not changed:
            return "no file of yours changed."
        tracked = subprocess.run(["git", "-C", str(self.repo_root), "diff", "--stat", "HEAD", "--", *changed],
                                 capture_output=True, text=True).stdout.strip()
        new = [rel for rel in changed if rel not in tracked]
        return "\n".join(filter(None, [tracked, *(f"new or untracked: {rel}" for rel in new)]))
