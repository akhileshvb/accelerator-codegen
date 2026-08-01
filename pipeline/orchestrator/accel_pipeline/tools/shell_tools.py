from __future__ import annotations

import os
import re
import signal
import subprocess
from dataclasses import dataclass

from accel_pipeline.config import PipelineConfig

_CTEST_DEFAULT_TIMEOUT_SECONDS = 240


@dataclass
class ToolContext:
    pipeline_config: PipelineConfig
    query: int


def make_shell_tools(ctx: ToolContext):
    allowed = [re.compile(p) for p in ctx.pipeline_config.allowed_commands]

    def run_command(cmd: str, cwd: str = "", timeout: int = 600) -> str:
        """Run an allowlisted build/test command."""
        if not any(p.match(cmd.strip()) for p in allowed):
            raise PermissionError(f"command not allowlisted: {cmd}")
        workdir = ctx.pipeline_config.repo_root
        if cwd:
            workdir = ctx.pipeline_config.repo_root / cwd
        effective_timeout = timeout
        if re.search(r"\bctest\b", cmd) and timeout >= 600:
            effective_timeout = _CTEST_DEFAULT_TIMEOUT_SECONDS
        proc = subprocess.Popen(
            cmd,
            shell=True,
            cwd=workdir,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        try:
            stdout, stderr = proc.communicate(timeout=effective_timeout)
            exit_code = proc.returncode
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                proc.kill()
            stdout, stderr = proc.communicate()
            out = (stdout or "") + (stderr or "")
            return (
                f"exit=124\n"
                f"command timed out after {effective_timeout}s (process group killed)\n"
                f"{out[-2000:]}"
            )
        out = (stdout or "") + (stderr or "")
        return f"exit={exit_code}\n{out[-2000:]}"

    return run_command
