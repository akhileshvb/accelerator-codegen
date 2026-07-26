from __future__ import annotations

from typing import Any, Callable

from accel_pipeline.config import PipelineConfig
from accel_pipeline.tools.gate_tools import ToolContext as GateCtx
from accel_pipeline.tools.gate_tools import make_gate_tools
from accel_pipeline.tools.repo_tools import ToolContext as RepoCtx
from accel_pipeline.tools.repo_tools import make_repo_tools
from accel_pipeline.tools.shell_tools import ToolContext as ShellCtx
from accel_pipeline.tools.shell_tools import make_shell_tools

STAGE_TOOL_NAMES: dict[str, list[str]] = {
    "analysis": ["read_file", "search_repo", "list_dir"],
    "implementation": [
        "read_file",
        "write_file",
        "apply_patch",
        "delete_file",
        "search_repo",
        "list_dir",
        "run_command",
        "request_gate",
    ],
    "optimization": [
        "read_file",
        "apply_patch",
        "write_file",
        "search_repo",
        "list_dir",
        "run_command",
        "request_gate",
    ],
    "audit": ["read_file", "search_repo", "list_dir", "request_gate"],
}


def make_search_repo(pipeline_config: PipelineConfig):
    import subprocess

    def search_repo(pattern: str, glob: str = "*", path: str = ".") -> str:
        """Search repo with ripgrep."""
        root = pipeline_config.repo_root / path
        proc = subprocess.run(
            ["rg", "-n", pattern, root, "-g", glob, "--max-count", "50"],
            capture_output=True,
            text=True,
            check=False,
        )
        return proc.stdout or proc.stderr

    return search_repo


def build_tools(
    pipeline_config: PipelineConfig,
    query: int,
    stage: str,
) -> list[Callable[..., Any]]:
    repo_ctx = RepoCtx(pipeline_config, query)
    shell_ctx = ShellCtx(pipeline_config, query)
    gate_ctx = GateCtx(pipeline_config, query)

    read_file, write_file, apply_patch, delete_file, list_dir = make_repo_tools(repo_ctx)
    run_command = make_shell_tools(shell_ctx)
    request_gate = make_gate_tools(gate_ctx)
    search_repo = make_search_repo(pipeline_config)

    registry = {
        "read_file": read_file,
        "write_file": write_file,
        "apply_patch": apply_patch,
        "delete_file": delete_file,
        "list_dir": list_dir,
        "run_command": run_command,
        "request_gate": request_gate,
        "search_repo": search_repo,
    }

    names = STAGE_TOOL_NAMES.get(stage, [])
    return [registry[n] for n in names if n in registry]


# TODO: OpenAI Agents SDK Guardrails on tool inputs (reject writes outside allowlist before execution).
