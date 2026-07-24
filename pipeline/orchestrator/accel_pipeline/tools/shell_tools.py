from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass

from accel_pipeline.config import PipelineConfig


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
        proc = subprocess.run(
            cmd,
            shell=True,
            cwd=workdir,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
        out = (proc.stdout or "") + (proc.stderr or "")
        return f"exit={proc.returncode}\n{out[-8000:]}"

    return run_command
