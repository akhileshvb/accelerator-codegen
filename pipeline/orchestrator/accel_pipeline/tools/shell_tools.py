from __future__ import annotations

import os
import re
import signal
import subprocess
from dataclasses import dataclass
from pathlib import Path

from accel_pipeline.config import PipelineConfig
from accel_pipeline.tools.tool_args import none_default as _none_default

_CTEST_DEFAULT_TIMEOUT_SECONDS = 240

# Read-only repo navigation; always permitted (does not need YAML allowlist entries).
_BUILTIN_DIAGNOSTIC_COMMANDS = [
    re.compile(r"^pwd$"),
    re.compile(r"^pwd && ls(?: [^|;&\n]*)?$"),
    re.compile(r"^ls$"),
    re.compile(r"^ls [^|;&\n]*$"),
    re.compile(r"^test -[ef] .+$"),
]


def _command_allowed(cmd: str, patterns: list[re.Pattern[str]]) -> bool:
    stripped = cmd.strip()
    if any(p.match(stripped) for p in _BUILTIN_DIAGNOSTIC_COMMANDS):
        return True
    return any(p.match(stripped) for p in patterns)


@dataclass
class ToolContext:
    pipeline_config: PipelineConfig
    query: int


def _build_dir_arg(pipeline_config: PipelineConfig) -> str:
    try:
        return str(pipeline_config.build_dir.relative_to(pipeline_config.repo_root))
    except ValueError:
        return str(pipeline_config.build_dir)


def _normalize_ctest_cmd(cmd: str, pipeline_config: PipelineConfig) -> str:
    stripped = cmd.strip()
    if not re.search(r"\bctest\b", stripped):
        return stripped
    if "--test-dir" in stripped or re.match(r"^cd\s+", stripped):
        return stripped
    build_dir = _build_dir_arg(pipeline_config)
    return re.sub(r"\bctest\b", f"ctest --test-dir {build_dir}", stripped, count=1)


def make_shell_tools(ctx: ToolContext):
    allowed = [re.compile(p) for p in ctx.pipeline_config.allowed_commands]

    def run_command(cmd: str, cwd: str | None = "", timeout: int | None = 600) -> str:
        """Run an allowlisted build/test command."""
        cwd = str(_none_default(cwd, ""))
        timeout = int(_none_default(timeout, 600))
        raw_cmd = cmd.strip()
        effective_cmd = _normalize_ctest_cmd(raw_cmd, ctx.pipeline_config)
        if not _command_allowed(raw_cmd, allowed) and not _command_allowed(effective_cmd, allowed):
            raise PermissionError(f"command not allowlisted: {cmd}")
        workdir = ctx.pipeline_config.repo_root
        if cwd:
            workdir = ctx.pipeline_config.repo_root / cwd
        effective_timeout = timeout
        if re.search(r"\bctest\b", effective_cmd) and timeout >= 600:
            effective_timeout = _CTEST_DEFAULT_TIMEOUT_SECONDS
        proc = subprocess.Popen(
            effective_cmd,
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
