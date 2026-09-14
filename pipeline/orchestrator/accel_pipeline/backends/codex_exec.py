"""Codex CLI backend (`codex exec`) with OpenRouter model provider + MCP tools."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Any

from accel_pipeline.backends.base import StageRunRequest, StageRunResult
from accel_pipeline.config import ModelConfig, PipelineConfig


def sidecar_env_line(key: str, value: Any) -> str:
    """KEY=value line safe to `source` from bash (spaces, colons, etc.)."""
    return f"{key}={shlex.quote(str(value))}"


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
        # Prefer per-worker CODEX_HOME when pack isolation is active.
        worker_home = getattr(self._pipeline_config, "codex_home", None)
        env_home = os.environ.get("ACCEL_PIPELINE_CODEX_HOME")
        if worker_home:
            path = Path(worker_home) / "config.toml"
            if path.is_file():
                return path
        if env_home:
            path = Path(env_home) / "config.toml"
            if path.is_file():
                return path
        raw = getattr(self._model_config, "codex_config_path", "pipeline/.codex/config.toml")
        path = Path(raw)
        if not path.is_absolute():
            path = self._pipeline_config.repo_root / path
        return path

    def _write_mcp_env_sidecar(self, codex_home: Path) -> Path:
        """Write ACCEL_* for the MCP launcher — Codex often sanitizes child env."""
        lines = [
            sidecar_env_line("ACCEL_PIPELINE_QUERY", self._query),
            sidecar_env_line("ACCEL_PIPELINE_STAGE", self._stage),
            sidecar_env_line("ACCEL_PIPELINE_WORKLOAD", self._workload_name),
            sidecar_env_line("ACCEL_PIPELINE_OPT_ITER", self._optimization_iter),
            sidecar_env_line("ACCEL_PIPELINE_REPO_ROOT", self._pipeline_config.repo_root),
            sidecar_env_line(
                "ACCEL_PIPELINE_WORKER_ID",
                getattr(self._pipeline_config, "worker_id", "") or self._query,
            ),
            sidecar_env_line("ACCEL_PIPELINE_BUILD_DIR", self._pipeline_config.build_dir),
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
            "ACCEL_DUCKDB_ROOT",
            "PG_CONFIG",
            "ACCEL_PG_CONFIG",
            "ACCEL_PG_BINDIR",
            "ACCEL_PG_ROOT",
            "ACCEL_PG_INCLUDEDIR_SERVER",
            "ACCEL_PG_LIBDIR",
            "ACCEL_PG_DISCOVERY",
        ):
            if os.environ.get(key):
                lines.append(sidecar_env_line(key, os.environ[key]))
        path = codex_home / "accel_pipeline_mcp.env"
        path.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return path

    @staticmethod
    def _engine_impl_mcp_tools(stage: str) -> list[str] | None:
        """Allow-list MCP tools Codex actually sees for engine_backend impl_*."""
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
        if os.environ.get("ACCEL_DUCKDB_ROOT"):
            env["ACCEL_DUCKDB_ROOT"] = os.environ["ACCEL_DUCKDB_ROOT"]
        if self._art_dir is not None:
            env["ACCEL_PIPELINE_ART_DIR"] = str(self._art_dir)

        # Codex loads model_providers from $CODEX_HOME/config.toml (user-level).
        # Project .codex/ configs ignore model_providers, so point CODEX_HOME at
        # the directory that contains our openrouter provider definition.
        if config_path.is_file():
            codex_home = config_path.parent
            env["CODEX_HOME"] = str(codex_home)
            self._write_mcp_env_sidecar(codex_home)

        with tempfile.TemporaryDirectory(prefix="accel_codex_") as tmp:
            task_path = Path(tmp) / "task.md"
            task_path.write_text(task, encoding="utf-8")
            # Force MCP env into Codex's mcp_servers config as well (belt + suspenders).
            # Include BUILD_DIR / WORKER_ID / CODEX_HOME — without these MCP falls
            # back to shared build-release and crosstalk returns.
            mcp_env_parts = [
                f'ACCEL_PIPELINE_QUERY="{self._query}"',
                f'ACCEL_PIPELINE_STAGE="{self._stage}"',
                f'ACCEL_PIPELINE_WORKLOAD="{self._workload_name}"',
                f'ACCEL_PIPELINE_OPT_ITER="{self._optimization_iter}"',
                f'ACCEL_PIPELINE_REPO_ROOT="{self._pipeline_config.repo_root}"',
                f'ACCEL_PIPELINE_BUILD_DIR="{self._pipeline_config.build_dir}"',
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
            if os.environ.get("ACCEL_DUCKDB_ROOT"):
                mcp_env_parts.append(
                    f'ACCEL_DUCKDB_ROOT="{os.environ["ACCEL_DUCKDB_ROOT"]}"'
                )
            for key in (
                "PG_CONFIG",
                "ACCEL_PG_CONFIG",
                "ACCEL_PG_BINDIR",
                "ACCEL_PG_ROOT",
                "ACCEL_PG_INCLUDEDIR_SERVER",
                "ACCEL_PG_LIBDIR",
            ):
                if os.environ.get(key):
                    mcp_env_parts.append(f'{key}="{os.environ[key]}"')
            mcp_env_toml = "{" + ", ".join(mcp_env_parts) + "}"
            cmd = [
                codex_bin,
                "exec",
                "--skip-git-repo-check",
                "-C",
                str(self._pipeline_config.repo_root),
                "-m",
                model,
                "-c",
                'model_provider="openrouter"',
                "-c",
                f"mcp_servers.accel_pipeline.env={mcp_env_toml}",
                "-c",
                'mcp_servers.accel_pipeline.default_tools_approval_mode="approve"',
            ]
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
            cmd.append(str(task_path))
            try:
                proc = subprocess.run(
                    cmd,
                    cwd=str(self._pipeline_config.repo_root),
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
                    metadata={"duration_ms": int((time.monotonic() - started) * 1000)},
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
            },
        )
