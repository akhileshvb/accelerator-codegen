"""Codex CLI backend (`codex exec`) with OpenRouter model provider + MCP tools."""

from __future__ import annotations

import json
import os
import signal
import sys
import shlex
import shutil
import subprocess
import re
import tempfile
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from accel_pipeline import advisor, session as session_mod
from accel_pipeline.backends.base import StageRunRequest, StageRunResult
from accel_pipeline.config import ModelConfig, PipelineConfig
from accel_pipeline.gates.run import TOOL_CALL_LIMIT_S
from accel_pipeline.advisor import READ_ONLY_TOOLS
from accel_pipeline.session import HEARTBEAT_FILE


def _forwarded_to_mcp(key: str) -> bool:
    """Engine, verus and mode settings reach the MCP server; the pipeline's own ACCEL_PIPELINE_* are set explicitly."""
    return key in ("ACCEL_PIPELINE_MODE", "CMAKE_BUILD_PARALLEL_LEVEL") or (key.startswith("ACCEL_") and not key.startswith("ACCEL_PIPELINE_"))


def sidecar_env_line(key: str, value: Any) -> str:
    """KEY=value line safe to `source` from bash (spaces, colons, etc.)."""
    return f"{key}={shlex.quote(str(value))}"


_CODEX_DIR = Path(__file__).resolve().parents[3] / "codex"
BLOCKER_STOP = "the agent reported a blocker (report_blocker)"


def ensure_codex_config(path: Path) -> Path:
    """Materialize a missing CODEX_HOME config from the committed template (atomically: homes are shared)."""
    if path.is_file():
        return path
    launcher = _CODEX_DIR / "run_mcp_server.sh"
    text = (_CODEX_DIR / "config.template.toml").read_text(encoding="utf-8")
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f"{path.name}.{os.getpid()}.{threading.get_ident()}.tmp")
    tmp.write_text(text.replace("{{mcp_launcher}}", str(launcher)), encoding="utf-8")
    os.replace(tmp, path)
    return path


@dataclass
class _Outcome:
    returncode: int | None
    stdout: str
    stderr: str
    killed: str = ""
    reason: str = ""  # why it was stopped: deadline | idle | no_progress | blocker | exit_gate (a finish, not a failure)
    detail: str = ""  # the evidence a directed resume carries


def _feed(proc: subprocess.Popen, text: str) -> None:
    try:
        proc.stdin.write(text)
        proc.stdin.close()
    except (BrokenPipeError, OSError):
        pass


def _kill(proc: subprocess.Popen) -> None:
    """Stop codex and everything it started (its MCP server, tools)."""
    for sig, grace in ((signal.SIGTERM, 10), (signal.SIGKILL, 10)):
        try:
            os.killpg(proc.pid, sig)
        except ProcessLookupError:
            return
        try:
            proc.wait(timeout=grace)
            return
        except subprocess.TimeoutExpired:
            continue


class CodexExecBackend:
    name = "codex_exec"

    def __init__(
        self,
        *,
        pipeline_config: PipelineConfig,
        model_config: ModelConfig,
        query: int,
        stage: str,
        workload_name: str = "",
        optimization_iter: int = 0,
        art_dir: Path | None = None,
    ) -> None:
        self._pipeline_config = pipeline_config
        self._model_config = model_config
        self._query = query
        self._stage = stage
        self._workload_name = workload_name
        self._optimization_iter = optimization_iter
        self._art_dir = art_dir

    def _resolve_codex_bin(self) -> str:
        configured = getattr(self._model_config, "codex_bin", "codex") or "codex"
        if Path(configured).is_file():
            return configured
        found = shutil.which(configured)
        if found:
            return found
        raise FileNotFoundError(
            f"codex binary not found ({configured!r}); install Codex CLI or set models.codex_bin"
        )

    def _config_path(self) -> Path:
        """The CODEX_HOME config: per-worker home, else ACCEL_PIPELINE_CODEX_HOME, else the configured path."""
        home = self._pipeline_config.codex_home or os.environ.get("ACCEL_PIPELINE_CODEX_HOME")
        if home:
            path = Path(home) / "config.toml"
        else:
            path = Path(self._model_config.codex_config_path)
            if not path.is_absolute():
                path = self._pipeline_config.repo_root / path
        return ensure_codex_config(path)

    def _write_mcp_env_sidecar(self, codex_home: Path) -> Path:
        """Write ACCEL_* for the MCP launcher — Codex often sanitizes child env."""
        lines = [
            sidecar_env_line("ACCEL_PIPELINE_QUERY", self._query),
            sidecar_env_line("ACCEL_PIPELINE_STAGE", self._stage),
            sidecar_env_line("ACCEL_PIPELINE_WORKLOAD", self._workload_name),
            sidecar_env_line("ACCEL_PIPELINE_ENGINE", self._pipeline_config.engine_name),
            sidecar_env_line("ACCEL_PIPELINE_OPT_ITER", self._optimization_iter),
            sidecar_env_line("ACCEL_PIPELINE_REPO_ROOT", self._pipeline_config.repo_root),
            sidecar_env_line(
                "ACCEL_PIPELINE_WORKER_ID",
                getattr(self._pipeline_config, "worker_id", "") or self._query,
            ),
            sidecar_env_line("ACCEL_PIPELINE_BUILD_DIR", self._pipeline_config.build_dir),
            # The MCP server runs on the orchestrator's own interpreter (its venv).
            sidecar_env_line("ACCEL_PIPELINE_PYTHON", sys.executable),
        ]
        if self._art_dir is not None:
            lines.append(sidecar_env_line("ACCEL_PIPELINE_ART_DIR", self._art_dir))
        if getattr(self._pipeline_config, "codex_home", None):
            lines.append(
                sidecar_env_line("ACCEL_PIPELINE_CODEX_HOME", self._pipeline_config.codex_home)
            )
        for key in (
            "ACCEL_PIPELINE_PRIMARY_REPO",
            "ACCEL_PIPELINE_PACK_QUERIES",
            "ACCEL_PIPELINE_ISOLATION_MODE",
        ):
            if os.environ.get(key):
                lines.append(sidecar_env_line(key, os.environ[key]))
        for key, value in os.environ.items():
            if _forwarded_to_mcp(key):
                lines.append(sidecar_env_line(key, value))
        path = codex_home / "accel_pipeline_mcp.env"
        # atomically: parallel pipelines share this CODEX_HOME, and a reader must never
        # see a half-written file
        tmp = path.with_name(f"{path.name}.{os.getpid()}.{threading.get_ident()}.tmp")  # stages run in threads
        tmp.write_text("\n".join(lines) + "\n", encoding="utf-8")
        os.replace(tmp, path)
        return path

    @staticmethod
    def _enabled_mcp_tools(stage: str) -> list[str] | None:
        """The MCP tools codex may call in `stage` (engine_backend layer and Verus steps restrict them), or None for all."""
        from accel_pipeline.tools import mode_tools

        mode = "verus" if stage.startswith("verus_") else "engine_backend" if stage.startswith("impl_") else None
        return mode_tools(mode).enabled_tools(stage) if mode else None

    def run_stage(self, request: StageRunRequest) -> StageRunResult:
        """One codex step, followed by the ownership check of what it changed (cfg.write_policy)."""
        cfg = self._pipeline_config
        policy = str(getattr(cfg, "write_policy", "enforce"))
        if policy == "off" or self._stage.startswith("verus_"):  # Verus steps work in a sandboxed workspace
            return self._run_codex(request)
        from accel_pipeline.layout import StageWriteCheck, violation_message

        check = StageWriteCheck(cfg.repo_root, query=self._query, stage=self._stage, pipeline_config=cfg)
        result = self._run_codex(request)
        bad = check.violations()
        if not bad:
            return result
        result.metadata["write_violations"] = [{"path": rel, "rule": why} for rel, why in bad]
        self._log_violations(policy, bad)
        if policy == "enforce":
            check.revert(bad)
            result.metadata.pop("timed_out", None)
            result.status = "error"
            result.error = "\n\n".join(filter(None, [violation_message(self._stage, bad, reverted=True), result.error]))
        return result

    def _log_violations(self, policy: str, bad: list[tuple[str, str]]) -> None:
        from accel_pipeline.workload import load_run_workload
        from accel_pipeline.pipeline_log import log_event

        try:
            workload = load_run_workload(self._pipeline_config, self._workload_name) if self._workload_name else None
        except Exception:  # logging must not fail the stage
            workload = None
        log_event(self._pipeline_config.repo_root, self._query, workload, "write_policy_violation",
                  stage=self._stage, policy=policy, paths=[rel for rel, _ in bad])

    def fork_stage(self, request: StageRunRequest) -> StageRunResult:
        """The advisor's turn: a fork of the session `request.metadata["fork_session_id"]` (a new session; that one is
        not resumed or changed). No write check: the caller undoes whatever the fork edits."""
        return self._run_codex(request)

    def _run_codex(self, request: StageRunRequest) -> StageRunResult:
        started = time.monotonic()
        started_wall = time.time()
        codex_bin = self._resolve_codex_bin()
        config_path = self._config_path()
        model = str(request.agent_model or request.model_id)
        fork = request.metadata.get("fork_session_id")
        resume = None if fork else request.metadata.get("resume_session_id") or getattr(self, "resume_session_id", None)
        self._live_session = str(resume or "")
        task = self._task_text(request, resumed=bool(resume), forked=bool(fork))

        env = os.environ.copy()
        env["ACCEL_PIPELINE_QUERY"] = str(self._query)
        env["ACCEL_PIPELINE_STAGE"] = self._stage
        env["ACCEL_PIPELINE_WORKLOAD"] = self._workload_name
        env["ACCEL_PIPELINE_ENGINE"] = self._pipeline_config.engine_name
        env["ACCEL_PIPELINE_OPT_ITER"] = str(self._optimization_iter)
        env["ACCEL_PIPELINE_REPO_ROOT"] = str(self._pipeline_config.repo_root)
        env["ACCEL_PIPELINE_BUILD_DIR"] = str(self._pipeline_config.build_dir)
        env["ACCEL_PIPELINE_WORKER_ID"] = str(
            getattr(self._pipeline_config, "worker_id", "") or self._query
        )
        if getattr(self._pipeline_config, "codex_home", None):
            env["ACCEL_PIPELINE_CODEX_HOME"] = str(self._pipeline_config.codex_home)
        if os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO"):
            env["ACCEL_PIPELINE_PRIMARY_REPO"] = os.environ["ACCEL_PIPELINE_PRIMARY_REPO"]
        if os.environ.get("ACCEL_PIPELINE_ISOLATION_MODE"):
            env["ACCEL_PIPELINE_ISOLATION_MODE"] = os.environ["ACCEL_PIPELINE_ISOLATION_MODE"]
        if os.environ.get("ACCEL_PIPELINE_PACK_QUERIES"):
            env["ACCEL_PIPELINE_PACK_QUERIES"] = os.environ["ACCEL_PIPELINE_PACK_QUERIES"]
        if self._art_dir is not None:
            env["ACCEL_PIPELINE_ART_DIR"] = str(self._art_dir)

        # Codex loads model_providers from $CODEX_HOME/config.toml (user-level), not from project configs.
        codex_home = config_path.parent
        env["CODEX_HOME"] = str(codex_home)
        self._write_mcp_env_sidecar(codex_home)

        # task_dir: where the task file goes when codex runs in a sandbox that
        # does not share the host's /tmp.
        task_dir = getattr(self, "task_dir", None)
        if task_dir is not None:
            Path(task_dir).mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="accel_codex_", dir=task_dir) as tmp:
            task_path = Path(tmp) / "task.md"
            task_path.write_text(task, encoding="utf-8")
            # Force MCP env into Codex's mcp_servers config as well (belt + suspenders).
            # Include BUILD_DIR / WORKER_ID / CODEX_HOME — without these MCP falls
            # back to shared build-release and crosstalk returns.
            mcp_env_parts = [
                f'ACCEL_PIPELINE_QUERY="{self._query}"',
                f'ACCEL_PIPELINE_STAGE="{self._stage}"',
                f'ACCEL_PIPELINE_WORKLOAD="{self._workload_name}"',
                f'ACCEL_PIPELINE_ENGINE="{self._pipeline_config.engine_name}"',
                f'ACCEL_PIPELINE_OPT_ITER="{self._optimization_iter}"',
                f'ACCEL_PIPELINE_REPO_ROOT="{self._pipeline_config.repo_root}"',
                f'ACCEL_PIPELINE_BUILD_DIR="{self._pipeline_config.build_dir}"',
                f'ACCEL_PIPELINE_PYTHON="{sys.executable}"',
                f'ACCEL_PIPELINE_WORKER_ID="{getattr(self._pipeline_config, "worker_id", "") or self._query}"',
            ]
            if self._art_dir is not None:
                mcp_env_parts.append(f'ACCEL_PIPELINE_ART_DIR="{self._art_dir}"')
            if getattr(self._pipeline_config, "codex_home", None):
                mcp_env_parts.append(
                    f'ACCEL_PIPELINE_CODEX_HOME="{self._pipeline_config.codex_home}"'
                )
            if os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO"):
                mcp_env_parts.append(
                    f'ACCEL_PIPELINE_PRIMARY_REPO="{os.environ["ACCEL_PIPELINE_PRIMARY_REPO"]}"'
                )
            if os.environ.get("ACCEL_PIPELINE_PACK_QUERIES"):
                mcp_env_parts.append(
                    f'ACCEL_PIPELINE_PACK_QUERIES="{os.environ["ACCEL_PIPELINE_PACK_QUERIES"]}"'
                )
            for key, value in os.environ.items():
                if _forwarded_to_mcp(key):
                    mcp_env_parts.append(f'{key}="{value}"')
            mcp_env_toml = "{" + ", ".join(mcp_env_parts) + "}"
            # resume: continue that codex session (its context kept) with this task as the
            # next message, instead of a fresh one.
            head = [codex_bin, "exec", "fork"] if fork else [codex_bin, "exec", "resume"] if resume else [codex_bin, "exec"]
            cmd = head + ["--skip-git-repo-check"] + ([] if resume or fork else ["-C", str(self._pipeline_config.repo_root)]) + [
                "-m",
                model,
                "-c",
                'model_provider="openrouter"',
                "-c",
                f"mcp_servers.accel_pipeline.env={mcp_env_toml}",
                "-c",
                'mcp_servers.accel_pipeline.default_tools_approval_mode="approve"',
                "-c",
                f"mcp_servers.accel_pipeline.tool_timeout_sec={TOOL_CALL_LIMIT_S}",
            ]
            if self._model_config.codex_model_context_window:
                cmd.extend(["-c", f"model_context_window={self._model_config.codex_model_context_window}"])
            if self._model_config.codex_auto_compact_tokens:
                cmd.extend(["-c", f"model_auto_compact_token_limit={self._model_config.codex_auto_compact_tokens}"])
            if self._model_config.codex_compact_prompt_file:
                compact = self._pipeline_config.repo_root / self._model_config.codex_compact_prompt_file
                cmd.extend(["-c", f'experimental_compact_prompt_file="{compact}"'])
            efforts = getattr(self._model_config, "stage_reasoning_effort", None) or {}
            effort = efforts.get(request.stage) or efforts.get(self._stage)
            if effort:
                cmd.extend(["-c", f'model_reasoning_effort="{effort}"'])
            enabled = READ_ONLY_TOOLS if fork else self._enabled_mcp_tools(self._stage)
            if enabled:
                listed = ", ".join(f'"{n}"' for n in enabled)
                cmd.extend(
                    ["-c", f"mcp_servers.accel_pipeline.enabled_tools=[{listed}]"]
                )
            # codex exec auto-cancels MCP tools when stdin can't approve
            # (openai/codex#16685). Bypass is required for first-party MCP in CI/pipeline.
            if bool(getattr(self._model_config, "codex_bypass_approvals_and_sandbox", True)):
                cmd.append("--dangerously-bypass-approvals-and-sandbox")
            else:
                cmd.extend(["-s", "workspace-write", "--approve-for-me"])
            if resume or fork:
                cmd.append(str(resume or fork))
            # prompt_via_stdin: the task text itself is the prompt (read from stdin), so the agent
            # does not have to read it back through a shell (whose output codex truncates).
            via_stdin = bool(fork) or bool(getattr(self, "prompt_via_stdin", False))
            cmd.append("-" if via_stdin else str(task_path))
            # An OS sandbox to run codex (and its MCP server) in, e.g. bwrap.
            cmd = list(getattr(self, "command_prefix", None) or []) + cmd
            try:
                outcome = self._supervise(cmd, env, task if via_stdin else None, started_wall, **({"watched": False} if fork else {}))
            except FileNotFoundError as exc:
                return StageRunResult(
                    backend=self.name,
                    stage=request.stage,
                    status="error",
                    error=str(exc),
                    metadata={"duration_ms": int((time.monotonic() - started) * 1000)},
                )

        duration_ms = int((time.monotonic() - started) * 1000)
        session_id = str(resume or "") or self._session_id(
            outcome.stdout + "\n" + outcome.stderr, env.get("CODEX_HOME"), started_wall, str(self._pipeline_config.repo_root))
        if outcome.killed:
            return StageRunResult(
                backend=self.name,
                stage=request.stage,
                status="error",
                error=outcome.killed,
                final_text=outcome.stdout[-200_000:],
                metadata={"duration_ms": duration_ms, "timed_out": True, "session_id": session_id,
                          "stop_reason": outcome.reason, "stop_detail": outcome.detail},
            )
        stdout, stderr = outcome.stdout, outcome.stderr
        combined = stdout if stdout.strip() else stderr
        status = "finished" if outcome.returncode == 0 else "error"
        return StageRunResult(
            backend=self.name,
            stage=request.stage,
            status=status,
            final_text=combined[-200_000:],
            error="" if status == "finished" else (stderr[-8000:] or f"exit={outcome.returncode}"),
            metadata={
                "duration_ms": duration_ms,
                "provider": self.name,
                "exit_code": outcome.returncode,
                "model": model,
                "config_path": str(config_path),
                "session_id": session_id,
            },
        )

    @staticmethod
    def _task_text(request: StageRunRequest, *, resumed: bool, forked: bool = False) -> str:
        """A fresh session gets the instructions and the task; a resumed one only the new message,
        with the instructions once when the stage is new to the session; a fork both."""
        message = request.user_message.rstrip()
        if forked:
            return f"{request.instructions.rstrip()}\n\n## Question\n\n{message}\n"
        if resumed and not request.metadata.get("phase_change"):
            return message + "\n"
        header = f"## NEW PHASE: {request.stage}\n\nFrom here on follow these instructions.\n\n" if resumed else ""
        return f"{header}{request.instructions.rstrip()}\n\n## Task\n\n{message}\n"

    def codex_home(self) -> Path:
        return self._config_path().parent

    def _deadlines(self) -> tuple[float, float]:
        """(idle timeout, hard deadline) in seconds; 0 disables. Verus steps keep codex_timeout_sec."""
        fallback = float(getattr(self._model_config, "codex_timeout_sec", 7200) or 7200)
        if getattr(self, "timeout_sec", None):  # the advisor's own limit
            return 0.0, float(self.timeout_sec)
        if self._stage.startswith("verus_"):
            return 0.0, fallback
        cfg = self._pipeline_config.session
        return float(cfg.idle_timeout_sec), float(cfg.hard_deadline_sec or fallback)

    def _supervise(self, cmd: list[str], env: dict[str, str], stdin: str | None, since: float,
                   watched: bool = True) -> _Outcome:
        """Run codex, killing its process group at the hard deadline or after the idle timeout (no
        output, rollout or heartbeat activity). `watched` = an agent turn (progress clock, exit gate, blocker,
        advisor requests); the advisor's fork is only held to its deadline."""
        idle, hard = self._deadlines()
        poll = max(0.05, min(5.0, (hard or 100.0) / 20, (idle or 100.0) / 10))
        art = self._art_dir if watched else None
        beat = art / HEARTBEAT_FILE if art is not None else None
        cwd = str(self._pipeline_config.repo_root)
        servicer = self._advisor_servicer() if watched else None
        with tempfile.TemporaryDirectory(prefix="accel_codex_out_") as tmp, \
                session_mod.agent_run(art, self._stage, self._pipeline_config):
            out_path, err_path = Path(tmp) / "stdout", Path(tmp) / "stderr"
            with out_path.open("w") as out, err_path.open("w") as err:
                proc = subprocess.Popen(cmd, cwd=cwd, stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL,
                                        stdout=out, stderr=err, text=True, env=env, start_new_session=True)
            if stdin is not None:
                threading.Thread(target=_feed, args=(proc, stdin), daemon=True).start()
            start = last_active = time.monotonic()
            clock = self._progress_clock(since) if watched else None
            rollout: Path | None = None
            killed = reason = detail = ""
            while True:
                try:
                    proc.wait(timeout=poll)
                    break
                except subprocess.TimeoutExpired:
                    pass
                now = time.monotonic()
                if rollout is None:
                    sid = self._session_id(err_path.read_text(errors="replace"), None, since)
                    rollout = session_mod.rollout_path(Path(env["CODEX_HOME"]), sid) if sid else None
                    self._live_session = sid or self._live_session
                touched = [p.stat().st_mtime for p in (out_path, err_path, rollout, beat) if p is not None and p.exists()]
                if touched:
                    last_active = max(last_active, now - (time.time() - max(touched)))
                if hard and now - start >= hard:
                    killed, reason = f"codex exec hit the hard deadline ({hard:.0f}s)", "deadline"
                elif idle and now - last_active >= idle:
                    killed, reason = f"codex exec idle for {idle:.0f}s (no output, session or tool activity)", "idle"
                elif watched and self._exit_gate_passed(since):
                    reason = "exit_gate"
                    _kill(proc)
                    break
                elif watched and (blocker := self._blocker_since(since)):
                    killed, reason, detail = BLOCKER_STOP, "blocker", blocker
                elif clock is not None and clock.poll(rollout) == "redirect":
                    killed = f"codex exec made no progress for {clock.idle_sec():.0f}s"
                    reason, detail = "no_progress", clock.redirect_report()
                if killed:
                    _kill(proc)
                    break
                if servicer is not None:
                    servicer.poll(self._live_session)
            if servicer is not None:
                servicer.join()
            if watched and not reason and (blocker := self._blocker_since(since)):  # the agent stopped as told
                killed, reason, detail = BLOCKER_STOP, "blocker", blocker
            return _Outcome(0 if reason == "exit_gate" else proc.returncode, out_path.read_text(errors="replace"),
                            err_path.read_text(errors="replace"), killed, reason, detail)

    def _advisor_servicer(self) -> advisor.Servicer | None:
        """Serves the agent's ask_advisor calls during its turn (stages with an artifact dir only)."""
        if self._art_dir is None or self._stage.startswith("verus_") or self._pipeline_config.advisor.max_consults_per_stage <= 0:
            return None
        return advisor.Servicer(self._pipeline_config, self._model_config, self._workload_name, self._query, self._stage,
                                self._optimization_iter, self._art_dir)

    def _progress_clock(self, since: float) -> session_mod.ProgressClock | None:
        """The turn's progress clock, when the stage is watched (a stage with no art dir or a Verus step is not)."""
        nudge, redirect = self._pipeline_config.session.no_progress.for_stage(self._stage)
        if self._art_dir is None or self._stage.startswith("verus_") or not (nudge or redirect):
            return None
        return session_mod.ProgressClock(
            self._art_dir, self._pipeline_config.repo_root, query=self._query, stage=self._stage,
            pipeline_config=self._pipeline_config, nudge_sec=nudge, redirect_sec=redirect, started_wall=since,
            sample_sec=max(0.05, min(15.0, (nudge or redirect) / 4)))

    def _exit_gate_passed(self, since: float) -> bool:
        """The stage's exit gate (config session.exit_gate) passed during this turn: its acceptance is met, so the
        turn ends (the agent would only keep polishing)."""
        gate = self._pipeline_config.session.exit_gate_for(self._stage)
        if not gate or self._art_dir is None:
            return False
        try:
            path = self._art_dir / "gates" / f"{gate}.json"
            return path.stat().st_mtime >= since and json.loads(path.read_text(encoding="utf-8")).get("status") == "pass"
        except (OSError, ValueError):
            return False

    def _blocker_since(self, since: float) -> str:
        """The report_blocker file written during this turn, else ''."""
        path = self._art_dir / session_mod.BLOCKER_FILE if self._art_dir is not None else None
        try:
            return path.read_text(encoding="utf-8") if path is not None and path.stat().st_mtime >= since else ""
        except OSError:
            return ""

    @staticmethod
    def _session_id(output: str, codex_home: str | None, since: float, cwd: str = "") -> str:
        """The codex session the run used: from its banner, else the newest session log since
        `since` started in `cwd` (other pipelines may share the codex home)."""
        m = re.search(r"session id:\s*([0-9a-fA-F-]{36})", output)
        if m:
            return m.group(1)
        if codex_home:
            logs = [q for q in (Path(codex_home) / "sessions").rglob("rollout-*.jsonl") if q.stat().st_mtime >= since]
            if cwd:
                logs = [q for q in logs if f'"cwd":"{cwd}"' in q.open(encoding="utf-8").readline()]
            if logs:
                newest = max(logs, key=lambda q: q.stat().st_mtime)
                m = re.search(r"([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})\.jsonl$",
                              newest.name)
                if m:
                    return m.group(1)
        return ""
