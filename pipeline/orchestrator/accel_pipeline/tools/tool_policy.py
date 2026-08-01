from __future__ import annotations

import subprocess
from typing import Any, Callable

from accel_pipeline.config import PipelineConfig, load_workload
from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.tools.ab_tools import AbToolContext, make_ab_tools
from accel_pipeline.tools.gate_tools import ToolContext as GateCtx
from accel_pipeline.tools.gate_tools import make_gate_tools
from accel_pipeline.tools.repo_tools import ToolContext as RepoCtx
from accel_pipeline.tools.repo_tools import make_repo_tools
from accel_pipeline.tools.reanalysis_tools import ToolContext as ReanalysisCtx
from accel_pipeline.tools.reanalysis_tools import make_reanalysis_tools
from accel_pipeline.tools.shell_tools import ToolContext as ShellCtx
from accel_pipeline.tools.shell_tools import make_shell_tools

STAGE_TOOL_NAMES: dict[str, list[str]] = {
    "analysis": ["read_file", "grep_context", "search_repo", "list_dir", "glob_files", "file_info"],
    "implementation": [
        "read_file",
        "grep_context",
        "write_file",
        "apply_patch",
        "insert_lines",
        "delete_file",
        "search_repo",
        "list_dir",
        "glob_files",
        "file_info",
        "run_command",
        "request_structural_reanalysis",
    ],
    "optimization": [
        "read_file",
        "grep_context",
        "apply_patch",
        "insert_lines",
        "write_file",
        "search_repo",
        "list_dir",
        "glob_files",
        "file_info",
        "run_command",
        "begin_ab_trial",
        "run_quick_benchmark",
        "end_ab_trial",
        "request_structural_reanalysis",
    ],
    "crash_debug": [
        "read_file",
        "grep_context",
        "apply_patch",
        "insert_lines",
        "write_file",
        "search_repo",
        "list_dir",
        "glob_files",
        "file_info",
        "run_command",
    ],
    "audit": ["read_file", "grep_context", "search_repo", "list_dir", "glob_files", "file_info", "request_gate"],
}

TOOL_USAGE_INSTRUCTIONS = """\
## Tooling

Use ONLY the pipeline tools for this stage. Do not use built-in shell/edit tools.

- read_file: default first 100 lines; use offset/limit for more. numbered=true for patches.
- grep_context: search one file with ±context — **prefer over read_file** on kernels/packages.
- apply_patch: old_string must match exactly once.
- run_command: build + ctest during implementation/optimization.
- begin_ab_trial / run_quick_benchmark / end_ab_trial: A/B test optimization bundles within an episode (profile-guided metrics returned).
- All gate/profile/benchmark history lives in `optimization_runtime.md` — do not re-read artifact JSON files.
- request_gate: audit stage only (speedup verification). Other gates run automatically after each stage.
"""


def make_search_repo(pipeline_config: PipelineConfig):
    def search_repo(
        pattern: str,
        glob: str = "*",
        path: str = ".",
        max_matches: int = 12,
    ) -> str:
        """Search repo with ripgrep."""
        root = pipeline_config.repo_root / path
        proc = subprocess.run(
            [
                "rg",
                "-n",
                pattern,
                str(root),
                "-g",
                glob,
                "--max-count",
                str(max_matches),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        output = proc.stdout or proc.stderr or "(no matches)"
        if proc.returncode not in (0, 1):
            return f"rg failed (exit={proc.returncode})\n{output[-8000:]}"
        return output

    return search_repo


def build_tool_registry(
    pipeline_config: PipelineConfig,
    query: int,
    *,
    stage: str = "",
    workload_name: str = "",
    optimization_iter: int = 0,
) -> dict[str, Callable[..., Any]]:
    repo_ctx = RepoCtx(pipeline_config, query)
    shell_ctx = ShellCtx(pipeline_config, query)
    gate_ctx = GateCtx(pipeline_config, query)
    reanalysis_ctx = ReanalysisCtx(
        pipeline_config, query, stage, workload_name, optimization_iter
    )

    (
        read_file,
        write_file,
        apply_patch,
        insert_lines,
        delete_file,
        list_dir,
        glob_files,
        file_info,
        grep_context,
    ) = make_repo_tools(repo_ctx)
    run_command = make_shell_tools(shell_ctx)
    request_gate = make_gate_tools(gate_ctx)
    request_structural_reanalysis = make_reanalysis_tools(reanalysis_ctx)
    search_repo = make_search_repo(pipeline_config)

    begin_ab_trial = run_quick_benchmark = end_ab_trial = lambda *args, **kwargs: "ERROR: A/B tools unavailable"
    if stage == "optimization" and workload_name:
        workload = load_workload(workload_name, pipeline_config.repo_root)
        ab_ctx = AbToolContext(
            pipeline_config=pipeline_config,
            query=query,
            workload=workload,
            optimization_iter=optimization_iter,
        )
        begin_ab_trial, run_quick_benchmark, end_ab_trial = make_ab_tools(ab_ctx)

    return {
        "read_file": read_file,
        "grep_context": grep_context,
        "write_file": write_file,
        "apply_patch": apply_patch,
        "insert_lines": insert_lines,
        "delete_file": delete_file,
        "list_dir": list_dir,
        "glob_files": glob_files,
        "file_info": file_info,
        "run_command": run_command,
        "begin_ab_trial": begin_ab_trial,
        "run_quick_benchmark": run_quick_benchmark,
        "end_ab_trial": end_ab_trial,
        "request_gate": request_gate,
        "request_structural_reanalysis": request_structural_reanalysis,
        "search_repo": search_repo,
    }


def build_tools(
    pipeline_config: PipelineConfig,
    query: int,
    stage: str,
    *,
    workload_name: str = "",
    optimization_iter: int = 0,
) -> list[Callable[..., Any]]:
    registry = build_tool_registry(
        pipeline_config,
        query,
        stage=stage,
        workload_name=workload_name,
        optimization_iter=optimization_iter,
    )
    names = STAGE_TOOL_NAMES.get(stage, [])
    return [registry[n] for n in names if n in registry]


def build_stage_tool_registry(
    pipeline_config: PipelineConfig,
    query: int,
    stage: str,
    *,
    workload_name: str = "",
    optimization_iter: int = 0,
) -> dict[str, Callable[..., Any]]:
    registry = build_tool_registry(
        pipeline_config,
        query,
        stage=stage,
        workload_name=workload_name,
        optimization_iter=optimization_iter,
    )
    names = STAGE_TOOL_NAMES.get(stage, [])
    return {name: registry[name] for name in names if name in registry}
