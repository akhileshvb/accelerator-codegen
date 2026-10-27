"""One agent turn: the codex step, its sandbox, session continuity and the budgets it runs under.

A stage builds a `StageRunRequest`, `create_agent_backend` returns the backend for the stage, and its `run_stage`
runs codex (with an OpenRouter model provider and the MCP server), supervises it (wall deadline, idle and
no-progress watchdog, exit gate, report_blocker) and checks what it wrote against the stage's ownership.

A role keeps one codex session across rounds and phases (resumed, or replaced with a handoff once retired). While a
turn runs, the supervisor keeps a `ProgressClock`: reads, greps and file views are activity; progress is a change
under the stage's owned paths, a stage artifact written, or a check (quick_check, a gate) whose result differs from
its previous one; in the analysis stages also a successful data-tool call (data_queries.jsonl), since measuring the
data is their work. The MCP tool footer tells the agent its wall budget and how long it has gone without progress,
and past `redirect_sec` the turn is ended with a directed resume. `report_blocker` is the agent's way out when the
obstacle is not in its files.

The sandbox: ``prefix(binds, cwd)`` is the mode-agnostic part: the system mounts, fresh /tmp /run /home, codex and
the binds a mode asks for. A bind is ``(mode, src, dst)``, mode ``rw`` | ``ro`` | ``symlink``; later binds win, so a
read-only layer re-bound over a read-write tree stays read-only. The Verus mounts (an allowlisted clean room) are built
in ``verus.context``; the C++ ones (``cpp_mounts``) are here. The sandbox cuts what an agent can write (substrate/, workloads/) and see;
ownership between queries and backends is checked after the stage (``layout.StageWriteCheck``).
"""
from __future__ import annotations

import fnmatch
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from contextlib import contextmanager, suppress
from dataclasses import dataclass, field, replace
from datetime import datetime
from pathlib import Path
from typing import Any, Callable, Iterator

from accel_pipeline import layout
from accel_pipeline.config import (
    read_json,
    HEARTBEAT_FILE,
    load_run_workload,
    ModelConfig,
    PipelineConfig,
    query_artifacts_dir,
    resolve_stage_provider,
    TOOL_CALL_LIMIT_S,
)
from accel_pipeline.data import host_info
from accel_pipeline.layout import (
    frozen_edited,
    layer_write_verdict,
    StageWriteCheck,
    tree_snapshot,
    violation_message,
)


@dataclass
class StageRunRequest:
    stage: str
    instructions: str
    user_message: str
    agent_name: str
    model_id: str
    agent_model: str | Any | None = None
    max_turns: int = 15
    # resume_session_id / phase_change: continue a codex session (the stage's instructions only on a phase change)
    metadata: dict[str, Any] = field(default_factory=dict)


@dataclass
class StageRunResult:
    backend: str
    stage: str
    status: str
    final_text: str = ""
    raw: Any = None
    run_id: str = ""
    error: str = ""
    metadata: dict[str, Any] = field(default_factory=dict)


# Stages that work on one query's artifacts: they get its artifact directory (the progress clock, deadline and
# blocker files, tool footers). Engine-level stages (host_bootstrap, validation, architecture_audit) have none.
_ARTIFACT_STAGES = frozenset(
    {
        "analysis",
        "analysis_critic",
        "test_suite",
        "implementation",
        "optimization_unified",
        "optimization_unified_hypothesis",
        "optimization_unified_patch",
        "optimization_worker",
        "optimization_fix",
        "crash_debug",
        "audit",
    }
)


def create_agent_backend(
    *,
    provider: str | None = None,
    pipeline_config: PipelineConfig,
    query: int,
    stage: str,
    model_config: ModelConfig,
    workload_name: str = "",
    optimization_iter: int = 0,
    art_dir: Path | None = None,
    command_prefix: list[str] | None = None,
) -> CodexExecBackend:
    """The backend for a stage. A mode with its own artifact directory and sandbox (Verus) passes `art_dir` and
    the bwrap `command_prefix` it built from `prefix`; otherwise they follow the stage and the config."""
    resolved = provider or resolve_stage_provider(stage, model_config)
    if resolved != "codex_exec":
        raise ValueError(f"unsupported provider: {resolved!r}")

    if art_dir is None and stage in _ARTIFACT_STAGES and workload_name:
        workload = load_run_workload(pipeline_config, workload_name)
        art_dir = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)

    backend = CodexExecBackend(
        pipeline_config=pipeline_config,
        model_config=model_config,
        query=query,
        stage=stage,
        workload_name=workload_name,
        optimization_iter=optimization_iter,
        art_dir=art_dir,
    )
    if command_prefix is not None:
        backend.command_prefix = command_prefix
        backend.prompt_via_stdin = True
    elif getattr(pipeline_config, "sandbox", "none") == "bwrap":
        backend.command_prefix = cpp_prefix(pipeline_config)
        backend.prompt_via_stdin = True  # the sandbox has its own /tmp: no task file to read back
    return backend


def _forwarded_to_mcp(key: str) -> bool:
    """Engine, verus and mode settings reach the MCP server; the pipeline's own ACCEL_PIPELINE_* are set explicitly."""
    return key in ("ACCEL_PIPELINE_MODE", "CMAKE_BUILD_PARALLEL_LEVEL") or (key.startswith("ACCEL_") and not key.startswith("ACCEL_PIPELINE_"))


def sidecar_env_line(key: str, value: Any) -> str:
    """KEY=value line safe to `source` from bash (spaces, colons, etc.)."""
    return f"{key}={shlex.quote(str(value))}"


_CODEX_DIR = Path(__file__).resolve().parents[2] / "codex"
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
        from accel_pipeline.mcp import mode_tools

        mode = "verus" if stage.startswith("verus_") else "engine_backend" if stage.startswith("impl_") else None
        return mode_tools(mode).enabled_tools(stage) if mode else None

    def run_stage(self, request: StageRunRequest) -> StageRunResult:
        """One codex step, followed by the ownership check of what it changed (cfg.write_policy)."""
        cfg = self._pipeline_config
        policy = str(getattr(cfg, "write_policy", "enforce"))
        if policy == "off" or self._stage.startswith("verus_"):  # Verus steps work in a sandboxed workspace
            return self._run_codex(request)
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
        from accel_pipeline.runlog import log_event

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
                agent_run(art, self._stage, self._pipeline_config):
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
                    rollout = rollout_path(Path(env["CODEX_HOME"]), sid) if sid else None
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

    def _advisor_servicer(self) -> Servicer | None:
        """Serves the agent's ask_advisor calls during its turn (stages with an artifact dir only)."""
        if self._art_dir is None or self._stage.startswith("verus_") or self._pipeline_config.advisor.max_consults_per_stage <= 0:
            return None
        from accel_pipeline.repair import Servicer  # repair runs agent turns, so it imports this module

        return Servicer(self._pipeline_config, self._model_config, self._workload_name, self._query, self._stage,
                                self._optimization_iter, self._art_dir)

    def _progress_clock(self, since: float) -> ProgressClock | None:
        """The turn's progress clock, when the stage is watched (a stage with no art dir or a Verus step is not)."""
        nudge, redirect = self._pipeline_config.session.no_progress.for_stage(self._stage)
        if self._art_dir is None or self._stage.startswith("verus_") or not (nudge or redirect):
            return None
        return ProgressClock(
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
        path = self._art_dir / BLOCKER_FILE if self._art_dir is not None else None
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


READ_ONLY_TOOLS = ["quick_check", "gate_status", "get_profile_digest", "get_store_diagnostics",
                   "get_optimization_evidence", "discover_host_sdk"]  # what the advisor fork may call
SESSIONS_FILE = "sessions.json"
META_FILE = "session_meta.json"
RUN_FILE = "agent_run.json"
PROGRESS_FILE = "agent_progress.json"  # the supervisor's progress clock

MAIN = "main"
READONLY = "readonly"
TESTER = "tester"  # the test_suite stage: its own session, never the implementer's

CHECKS_FILE = "check_signatures.json"  # written by the MCP server: {check: signature of its last result}
BLOCKER_FILE = "blocker.md"
DATA_LOG = "data_queries.jsonl"  # the data tools in mcp.py

CHECK_TOOLS = frozenset({"quick_check", "request_gate", "run_engine_build", "run_layer_gate"})
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


def write_json(path: Path, data: dict[str, Any]) -> None:
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
        return str(read_json(self._ids).get(str(query), {}).get(role) or "")

    def resumes(self, query: int, role: str) -> int:
        return int(self._role_meta(query, role).get("resumes", 0))

    def phases(self, query: int, role: str) -> list[str]:
        return list(self._role_meta(query, role).get("phases", []))

    def record(self, query: int, role: str, session_id: str, phase: str, *, resumed: bool) -> None:
        ids = read_json(self._ids)
        ids.setdefault(str(query), {})[role] = session_id
        write_json(self._ids, ids)
        meta = read_json(self._meta)
        entry = meta.setdefault(str(query), {}).setdefault(role, {})
        if resumed:
            entry["resumes"] = int(entry.get("resumes", 0)) + 1
        else:
            entry.update(resumes=0, phases=[])
        if phase not in entry.setdefault("phases", []):
            entry["phases"].append(phase)
        write_json(self._meta, meta)

    def reset(self, query: int, role: str) -> None:
        for path in (self._ids, self._meta):
            data = read_json(path)
            if data.get(str(query), {}).pop(role, None) is not None:
                write_json(path, data)

    def _role_meta(self, query: int, role: str) -> dict[str, Any]:
        return read_json(self._meta).get(str(query), {}).get(role, {})


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
    manifest = read_json(art / "best_kernel_snapshot" / "manifest.json")
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
    write_json(art / RUN_FILE, {"stage": stage, "started": time.time(),
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
    run = read_json(Path(art) / RUN_FILE) if art else {}
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
    progress = read_json(art / PROGRESS_FILE)
    nudge = float((read_json(art / RUN_FILE).get("no_progress") or {}).get("nudge_sec") or 0)
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
        write_json(self.art / PROGRESS_FILE, {"last_progress": time.time() - self.idle_sec(),
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


HOME = Path.home()
# Repo layers mounted read-only inside the sandbox (see accel_pipeline.layout).
READONLY_LAYERS = ("substrate", "workloads")
DOCKER_SOCKET = Path("/run/docker.sock")

Bind = tuple[str, Path, Path]


def codex_package() -> list[Path]:
    """The codex launcher and the package it resolves to."""
    out: list[Path] = []
    exe = shutil.which("codex")
    if exe:
        out.append(Path(exe))
        out.append(Path(os.path.realpath(exe)).parent.parent)  # .../@openai/codex
    return out


def codex_binds() -> list[Bind]:
    # the launcher stays a symlink (codex finds its native binary from its real path)
    return [("symlink", Path(os.readlink(p)), p) if p.is_symlink() else ("ro", p, p) for p in codex_package()]


def readonly_layer_binds(root: Path, layers: Any) -> list[Bind]:
    """Layers no agent may write, re-bound read-only over the tree."""
    return [("ro", p, p) for p in (root / str(layer) for layer in layers or []) if p.is_dir()]


def python_prefix_binds(root: Path) -> list[Bind]:
    """The orchestrator's python environment when it lives under $HOME outside the tree."""
    return [("ro", p, p) for p in {Path(sys.prefix).resolve(), Path(sys.base_prefix).resolve()}
            if p.is_relative_to(HOME) and not p.is_relative_to(root)]


def extra_binds(mode: str, paths: Any) -> list[Bind]:
    return [(mode, Path(x).resolve(), Path(x).resolve()) for x in paths or []]


def prefix(binds: list[Bind], cwd: Path, *, setting: str = "sandbox") -> list[str]:
    """The bwrap command line that runs a command in the sandbox (append the command)."""
    bwrap = shutil.which("bwrap")
    if not bwrap:
        raise RuntimeError(f"{setting} is bwrap but bubblewrap is not installed (or set {setting}: none)")
    args = [bwrap, "--unshare-all", "--share-net", "--die-with-parent",
            "--ro-bind", "/usr", "/usr", "--ro-bind", "/etc", "/etc"]
    for link in ("bin", "lib", "lib64", "sbin"):
        p = Path("/") / link
        if p.is_symlink():
            args += ["--symlink", os.readlink(p), str(p)]
        elif p.is_dir():
            args += ["--ro-bind", str(p), str(p)]
    if Path("/opt").is_dir():
        args += ["--ro-bind", "/opt", "/opt"]
    # /sys read-only: without it perf finds no PMU (every profile came back "not applicable")
    args += ["--proc", "/proc", "--dev", "/dev", "--ro-bind", "/sys", "/sys", "--tmpfs", "/tmp", "--tmpfs", "/run"]
    # /etc/resolv.conf may point into /run (systemd-resolved)
    resolv = Path(os.path.realpath("/etc/resolv.conf"))
    if resolv.is_relative_to("/run") and resolv.parent.is_dir():
        args += ["--ro-bind", str(resolv.parent), str(resolv.parent)]
    args += ["--tmpfs", "/home"]
    if not HOME.is_relative_to("/home"):
        args += ["--tmpfs", str(HOME)]
    args += ["--dir", str(HOME)]
    for mode, src, dst in binds:
        if mode == "symlink":
            args += ["--symlink", str(src), str(dst)]
        elif mode == "tmpfs":
            args += ["--tmpfs", str(dst)]
        else:
            args += ["--bind" if mode == "rw" else "--ro-bind", str(src), str(dst)]
    args += ["--chdir", str(cwd)]
    return args


# ---------------------------------------------------------------- C++ mode


def _covered(path: Path, binds: list[Bind]) -> bool:
    return any(path.is_relative_to(dst) for _, _, dst in binds)


def _add(binds: list[Bind], mode: str, path: Path | str | None) -> None:
    """Bind an existing path at its real location unless a bind already holds it."""
    if not path or not Path(path).exists():
        return
    real = Path(path).resolve()
    if not _covered(real, binds):
        binds.append((mode, real, real))


def _add_dataset(binds: list[Bind], path: Path) -> None:
    """A dataset is never writable: its directory read-only, or, where a writable bind already holds it
    (a dataset inside the tree or the build dir), the dataset itself re-bound read-only over that."""
    if any(m == "ro" and path.is_relative_to(dst) for m, _, dst in binds):
        return
    if _covered(path, [b for b in binds if b[0] == "rw"]):
        binds.append(("ro", path, path))
    else:
        _add(binds, "ro", path if path.is_dir() else path.parent)


def backend_needs(cfg: Any) -> dict[str, Any] | None:
    """What the engine's backend says its verbs need (host.sh `info`: mounts, docker); None without a host.sh."""
    if not cfg.engine_name:
        return None
    return host_info(Path(cfg.repo_root), cfg.engine_name, Path(cfg.build_dir))


def _primary(cfg: Any) -> Path:
    return Path(os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO") or cfg.repo_root).resolve()


def cpp_mounts(cfg: Any) -> list[Bind]:
    """(mode, source, destination) for every bind of a C++-mode agent step beyond the system ones.

    The run's tree is read-write with substrate/ and workloads/ read-only over it; a pack worker's
    worktree sits under the primary checkout, which the MCP server also writes (primitive_publish,
    shared artifacts), so both are mounted, and the other workers' worktrees are hidden.
    """
    root = Path(cfg.repo_root).resolve()
    primary = _primary(cfg)
    layers = list(READONLY_LAYERS)
    binds: list[Bind] = []
    if primary != root:
        binds.append(("rw", primary, primary))
        binds += readonly_layer_binds(primary, layers)
        worktrees = primary / "pipeline" / "worktrees"
        if root.is_relative_to(worktrees):
            binds.append(("tmpfs", worktrees, worktrees))
    binds.append(("rw", root, root))
    binds += readonly_layer_binds(root, layers)
    for p in (cfg.build_dir, cfg.codex_home, os.environ.get("ACCEL_ENGINE_STATE_DIR")):
        if p:
            Path(p).resolve().mkdir(parents=True, exist_ok=True)  # resolve: a build-* symlink may dangle
            _add(binds, "rw", p)
    needs = backend_needs(cfg) or {}
    for mode in ("ro", "rw"):
        for d in (needs.get("mounts") or {}).get(mode) or []:
            _add(binds, mode, d)
    for f in [*cfg.datasets.values(), os.environ.get("ACCEL_DATASET_DIR")]:
        if f and Path(f).exists():
            _add_dataset(binds, Path(f).resolve())
    if needs.get("docker"):
        _add(binds, "rw", DOCKER_SOCKET)
        if DOCKER_SOCKET.exists() and Path("/var/run").is_symlink():  # the docker CLI's default path
            binds.append(("symlink", Path("/run"), Path("/var/run")))
    if (HOME / ".cache" / "ccache").is_dir():
        _add(binds, "rw", HOME / ".cache" / "ccache")
    _add(binds, "rw", os.environ.get("CCACHE_DIR"))
    binds += codex_binds()
    binds += [b for b in python_prefix_binds(root) if not _covered(b[2], binds)]
    binds += extra_binds("ro", getattr(cfg, "sandbox_extra_ro", []))
    binds += extra_binds("rw", getattr(cfg, "sandbox_extra_rw", []))
    return binds


def cpp_prefix(cfg: Any) -> list[str]:
    return prefix(cpp_mounts(cfg), Path(cfg.repo_root))


def cpp_selfcheck(cfg: Any, query: int) -> dict[str, Any]:
    """Run the sandbox: the tree and tools are there, substrate/ is read-only, the run's own
    directory and the build dir are writable, the engine's host script is reachable."""
    root = Path(cfg.repo_root)
    if cfg.workload_kind == "engine_backend":
        own = root / "backends" / cfg.engine_name
    else:
        own = root / layout.query_dir(cfg.benchmark_name, query)
    while not own.exists() and own != root:
        own = own.parent
    tag = f".sandbox_probe_{uuid.uuid4().hex[:8]}"
    frozen = f"{root}/{layout.SUBSTRATE}/{tag}"
    build = Path(cfg.build_dir)
    build.mkdir(parents=True, exist_ok=True)
    lines = [f'[ -e "{root}/pipeline" ] || echo "MISSING tree {root}"',
             'command -v codex >/dev/null || echo "MISSING codex"',
             f'if touch "{frozen}" 2>/dev/null; then rm -f "{frozen}"; echo "WRITABLE {layout.SUBSTRATE}"; fi',
             f'touch "{own}/{tag}" 2>/dev/null && rm -f "{own}/{tag}" || echo "READONLY {own}"',
             f'touch "{build}/{tag}" 2>/dev/null && rm -f "{build}/{tag}" || echo "READONLY build dir {build}"']
    needs = backend_needs(cfg)
    notes = []
    if needs is None and cfg.engine_name:  # no host.sh yet (early engine_backend layers): nothing extra mounted
        notes.append(f"backend {cfg.engine_name} has no usable host.sh info: no backend mounts or docker socket")
    elif needs is not None:
        lines.append(f'[ -x "{root}/backends/{cfg.engine_name}/host.sh" ] || echo "MISSING engine host script"')
        for mode in ("ro", "rw"):
            lines += [f'[ -e "{d}" ] || echo "MISSING backend mount {d}"' for d in (needs.get("mounts") or {}).get(mode) or []]
        if needs.get("docker"):
            lines.append(f'[ -S "{DOCKER_SOCKET}" ] || echo "MISSING docker socket {DOCKER_SOCKET}"')
    lines.append("true")
    proc = subprocess.run(cpp_prefix(cfg) + ["bash", "-c", "\n".join(lines)], capture_output=True, text=True,
                          timeout=60)
    problems = [l for l in proc.stdout.splitlines() if l.strip()]
    if proc.returncode != 0:
        problems.append(f"sandbox shell failed (exit {proc.returncode}): {proc.stderr[-500:]}")
    return {"ok": not problems, "problems": problems, "notes": notes}
