"""Codex CLI backend (`codex exec`) with OpenRouter model provider + MCP tools."""

from __future__ import annotations

import os
import sys
import shlex
import shutil
import subprocess
import re
import tempfile
import threading
import time
from pathlib import Path
from typing import Any

from accel_pipeline.backends.base import StageRunRequest, StageRunResult
from accel_pipeline.config import ModelConfig, PipelineConfig


def _forwarded_to_mcp(key: str) -> bool:
    """Engine, verus and mode settings reach the MCP server; the pipeline's own ACCEL_PIPELINE_* are set explicitly."""
    return key == "ACCEL_PIPELINE_MODE" or (key.startswith("ACCEL_") and not key.startswith("ACCEL_PIPELINE_"))


def sidecar_env_line(key: str, value: Any) -> str:
    """KEY=value line safe to `source` from bash (spaces, colons, etc.)."""
    return f"{key}={shlex.quote(str(value))}"


_CODEX_DIR = Path(__file__).resolve().parents[3] / "codex"


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
    def _engine_impl_mcp_tools(stage: str) -> list[str] | None:
        """Allow-list MCP tools Codex actually sees for engine_backend impl_* and Verus steps."""
        if str(stage or "").startswith("verus_"):
            from accel_pipeline.verus.tools import stage_tools

            return stage_tools(stage)
        if not str(stage or "").startswith("impl_"):
            return None
        return [
            "request_gate",
            "run_engine_build",
            "run_layer_gate",
            "discover_host_sdk",
            "acquire_shared_lease",
            "release_shared_lease",
        ]

    def run_stage(self, request: StageRunRequest) -> StageRunResult:
        started = time.monotonic()
        started_wall = time.time()
        codex_bin = self._resolve_codex_bin()
        config_path = self._config_path()
        timeout = int(getattr(self._model_config, "codex_timeout_sec", 7200) or 7200)
        model = str(request.agent_model or request.model_id)

        task = (
            f"{request.instructions.rstrip()}\n\n"
            f"## Task\n\n{request.user_message.rstrip()}\n"
        )

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
            # resume_session_id: continue that codex session (its context kept)
            # with this task as the next message, instead of a fresh one.
            resume = getattr(self, "resume_session_id", None)
            head = [codex_bin, "exec", "resume"] if resume else [codex_bin, "exec"]
            cmd = head + ["--skip-git-repo-check"] + ([] if resume else ["-C", str(self._pipeline_config.repo_root)]) + [
                "-m",
                model,
                "-c",
                'model_provider="openrouter"',
                "-c",
                f"mcp_servers.accel_pipeline.env={mcp_env_toml}",
                "-c",
                'mcp_servers.accel_pipeline.default_tools_approval_mode="approve"',
            ]
            effort = (getattr(self._model_config, "stage_reasoning_effort", None) or {}).get(self._stage)
            if effort:
                cmd.extend(["-c", f'model_reasoning_effort="{effort}"'])
            enabled = self._engine_impl_mcp_tools(self._stage)
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
            if resume:
                cmd.append(str(resume))
            # prompt_via_stdin: the task text itself is the prompt (read from stdin), so the agent
            # does not have to read it back through a shell (whose output codex truncates).
            via_stdin = bool(getattr(self, "prompt_via_stdin", False))
            cmd.append("-" if via_stdin else str(task_path))
            # An OS sandbox to run codex (and its MCP server) in, e.g. bwrap.
            cmd = list(getattr(self, "command_prefix", None) or []) + cmd
            try:
                proc = subprocess.run(
                    cmd,
                    cwd=str(self._pipeline_config.repo_root),
                    input=task if via_stdin else None,
                    capture_output=True,
                    text=True,
                    timeout=timeout,
                    env=env,
                    check=False,
                )
            except subprocess.TimeoutExpired as exc:
                return StageRunResult(
                    backend=self.name,
                    stage=request.stage,
                    status="error",
                    error=f"codex exec timed out after {timeout}s",
                    final_text=(exc.stdout or "") if isinstance(exc.stdout, str) else "",
                    # the session (to continue it) from its log: the banner went with the killed process
                    metadata={"duration_ms": int((time.monotonic() - started) * 1000), "timed_out": True,
                              "session_id": str(resume or "") or self._session_id(
                                  "", env.get("CODEX_HOME"), started_wall, str(self._pipeline_config.repo_root))},
                )
            except FileNotFoundError as exc:
                return StageRunResult(
                    backend=self.name,
                    stage=request.stage,
                    status="error",
                    error=str(exc),
                    metadata={"duration_ms": int((time.monotonic() - started) * 1000)},
                )

        stdout = proc.stdout or ""
        stderr = proc.stderr or ""
        combined = stdout if stdout.strip() else stderr
        status = "finished" if proc.returncode == 0 else "error"
        session_id = self._session_id(stdout + "\n" + stderr, env.get("CODEX_HOME"), started_wall,
                                      str(self._pipeline_config.repo_root))
        return StageRunResult(
            backend=self.name,
            stage=request.stage,
            status=status,
            final_text=combined[-200_000:],
            error="" if status == "finished" else (stderr[-8000:] or f"exit={proc.returncode}"),
            metadata={
                "duration_ms": int((time.monotonic() - started) * 1000),
                "provider": self.name,
                "exit_code": proc.returncode,
                "model": model,
                "config_path": str(config_path),
                "session_id": session_id,
            },
        )

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
