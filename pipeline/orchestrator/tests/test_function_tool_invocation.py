from __future__ import annotations

import asyncio
import json
from typing import Any

import pytest

from accel_pipeline.config import apply_workload_overrides, load_pipeline_config, load_workload
from accel_pipeline.paths import find_repo_root
from accel_pipeline.tools.pipeline_tool_wrapper import wrap_pipeline_tool
from accel_pipeline.tools.tool_policy import STAGE_TOOL_NAMES, build_tool_registry

_SDK_TOOL_FAILURE = "An error occurred while running the tool"
_SIGNATURE_FAILURE = "takes 0 positional arguments but"

ALL_TOOL_NAMES = sorted({name for names in STAGE_TOOL_NAMES.values() for name in names})

MINIMAL_ARGS: dict[str, dict[str, Any]] = {
    "read_file": {"path": "pipeline/README.md"},
    "grep_context": {"path": "pipeline/README.md", "pattern": "pipeline"},
    "write_file": {"path": "pipeline/artifacts/tpch/q18/tool_invoke_test.txt", "content": "tool test\n"},
    "search_repo": {"pattern": "pipeline", "path": "pipeline", "max_matches": 1},
    "rip_grep": {"pattern": "pipeline", "path": "pipeline", "max_matches": 1, "context": 1},
    "ast_grep": {"pattern": "pipeline", "path": "pipeline", "lang": "md", "max_matches": 1},
    "list_dir": {"path": "pipeline"},
    "glob_files": {"pattern": "README.md", "path": "pipeline"},
    "file_info": {"path": "pipeline/README.md"},
    # nothing staged: the tool must answer with a rejection, not crash
    "primitive_publish": {
        "files": ["include/accelerator_codegen/stateful/prims/__not_staged__.hpp"],
        "rationale": "tool invocation test: nothing is staged",
    },
    "apply_patch": {
        "path": "pipeline/artifacts/tpch/q18/tool_invoke_test.txt",
        "old_string": "tool test\n",
        "new_string": "tool test patched\n",
    },
    "insert_lines": {
        "path": "pipeline/artifacts/tpch/q18/tool_invoke_test.txt",
        "after_line": 1,
        "content": "# inserted",
    },
    "delete_file": {"path": "pipeline/artifacts/tpch/q18/tool_invoke_test.txt"},
    "read_reference_files": {
        "paths": [
            "pipeline/prompts/aux_state/shared/module_layout.md",
            "config/q18_aux_cost_model.json",
        ],
        "limit_per_file": 30,
    },
    "run_command": {"cmd": "echo pipeline-tool-test"},
    "request_structural_reanalysis": {
        "bottleneck_type": "other",
        "evidence": "Synthetic tool invocation test evidence string long enough.",
        "proposed_direction": "Synthetic proposed direction for tool test.",
        "current_candidate_id": "cand_test",
    },
    "begin_ab_trial": {
        "trial_id": "tool_test_trial",
        "hypothesis": "Synthetic A/B trial for function_tool invocation test.",
    },
    "run_quick_benchmark": {"include_profile": False},
    "end_ab_trial": {"trial_id": "tool_test_trial", "keep": False},
    "request_gate": {"gate_name": "scaffold"},
}

HEAVY_TOOLS = frozenset({"request_gate", "run_quick_benchmark", "begin_ab_trial", "end_ab_trial"})
IMPLEMENTATION_ONLY_TOOLS = frozenset({"read_reference_files"})

NULL_OPTIONAL_ARGS: dict[str, dict[str, Any]] = {
    "read_file": {"offset": None, "limit": None, "numbered": None},
    "grep_context": {"context": None, "max_matches": None, "case_insensitive": None},
    "search_repo": {"glob": None, "path": None, "max_matches": None},
    "rip_grep": {"path": None, "glob": None, "context": None, "max_matches": None, "case_insensitive": None},
    "ast_grep": {"path": None, "lang": None, "glob": None, "max_matches": None},
    "list_dir": {"path": None, "glob": None},
    "glob_files": {"path": None, "limit": None},
    "run_command": {"cwd": None, "timeout": None},
    "run_quick_benchmark": {"include_profile": None},
    "request_structural_reanalysis": {
        "alternate_candidate_ids": None,
        "cost_model_insufficient_reason": None,
    },
}


@pytest.fixture(scope="module")
def tool_registry():
    repo = find_repo_root()
    pipeline_config = load_pipeline_config(repo_root=repo)
    workload = load_workload("tpch_q18_aux", repo)
    pipeline_config = apply_workload_overrides(pipeline_config, workload)
    return build_tool_registry(
        pipeline_config,
        18,
        stage="optimization",
        workload_name="tpch_q18_aux",
        optimization_iter=1,
    )


def _invoke_function_tool(fn: Any, tool_name: str, args: dict[str, Any]) -> str:
    from agents import function_tool
    from agents.tool_context import ToolContext as AgentToolContext

    wrapped = wrap_pipeline_tool(fn, tool_name)
    tool = function_tool(wrapped)

    async def run() -> str:
        payload = json.dumps(args)
        ctx = AgentToolContext(
            context=None,
            tool_name=tool_name,
            tool_call_id="test",
            tool_arguments=payload,
        )
        return await tool.on_invoke_tool(ctx, payload)

    return asyncio.run(run())


def _assert_no_sdk_dispatch_error(result: str) -> None:
    assert _SIGNATURE_FAILURE not in result
    assert not result.startswith(_SDK_TOOL_FAILURE)


@pytest.mark.parametrize("tool_name", [n for n in ALL_TOOL_NAMES if n not in HEAVY_TOOLS and n not in IMPLEMENTATION_ONLY_TOOLS])
def test_all_tools_invoke_via_function_tool(tool_name: str, tool_registry: dict[str, Any]) -> None:
    fn = tool_registry[tool_name]
    args = dict(MINIMAL_ARGS[tool_name])
    result = _invoke_function_tool(fn, tool_name, args)
    _assert_no_sdk_dispatch_error(result)


@pytest.mark.parametrize("tool_name", sorted(IMPLEMENTATION_ONLY_TOOLS))
def test_implementation_only_tools_invoke_via_function_tool(
    tool_name: str, tool_registry: dict[str, Any]
) -> None:
    fn = tool_registry[tool_name]
    args = dict(MINIMAL_ARGS[tool_name])
    result = _invoke_function_tool(fn, tool_name, args)
    _assert_no_sdk_dispatch_error(result)
    assert "module_layout" in result or "q18_aux_cost_model" in result


@pytest.mark.parametrize("tool_name", sorted(HEAVY_TOOLS))
def test_heavy_tools_invoke_via_function_tool(
    tool_name: str, tool_registry: dict[str, Any], monkeypatch: pytest.MonkeyPatch
) -> None:
    if tool_name == "request_gate":
        from accel_pipeline.gates import GateResult

        monkeypatch.setattr(
            "accel_pipeline.tools.gate_tools.run_gate",
            lambda *args, **kwargs: GateResult(name="scaffold", status="pass", data={"status": "pass"}),
        )
    if tool_name == "run_quick_benchmark":
        import subprocess

        class _Completed:
            returncode = 0
            stdout = "speedup_median=1.0\n"
            stderr = ""

        monkeypatch.setattr(subprocess, "run", lambda *args, **kwargs: _Completed())
    if tool_name in {"begin_ab_trial", "end_ab_trial"}:
        monkeypatch.setattr(
            "accel_pipeline.tools.ab_tools._checkpoint_files",
            lambda ctx: [ctx.pipeline_config.repo_root / "pipeline/README.md"],
        )
        if tool_name == "end_ab_trial":
            import accel_pipeline.tools.ab_tools as ab_tools

            monkeypatch.setattr(
                ab_tools,
                "_read_active_trial",
                lambda ctx: {
                    "trial_id": "tool_test_trial",
                    "hypothesis": "Synthetic A/B trial for function_tool invocation test.",
                    "files": ["pipeline/README.md"],
                    "last_metrics": {},
                },
            )
            monkeypatch.setattr(ab_tools.subprocess, "run", lambda *args, **kwargs: type("R", (), {"returncode": 0, "stdout": "", "stderr": ""})())

    fn = tool_registry[tool_name]
    args = dict(MINIMAL_ARGS[tool_name])
    result = _invoke_function_tool(fn, tool_name, args)
    _assert_no_sdk_dispatch_error(result)


@pytest.mark.parametrize("tool_name", sorted(NULL_OPTIONAL_ARGS))
def test_optional_null_args_invoke_via_function_tool(
    tool_name: str, tool_registry: dict[str, Any]
) -> None:
    fn = tool_registry[tool_name]
    args = {**MINIMAL_ARGS[tool_name], **NULL_OPTIONAL_ARGS[tool_name]}
    result = _invoke_function_tool(fn, tool_name, args)
    _assert_no_sdk_dispatch_error(result)


def test_wrap_pipeline_tool_forwards_positional_args() -> None:
    def sample(path: str, count: int | None = 2) -> str:
        return f"{path}:{count}"

    wrapped = wrap_pipeline_tool(sample, "sample")
    assert wrapped("pipeline/README.md", 3) == "pipeline/README.md:3"
    assert wrapped("pipeline/README.md", None) == "pipeline/README.md:2"
