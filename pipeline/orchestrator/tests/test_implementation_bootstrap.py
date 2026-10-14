from __future__ import annotations

from accel_pipeline.config import apply_workload_overrides, load_pipeline_config, load_workload
from accel_pipeline.context import build_prompt_context
from accel_pipeline.implementation_bootstrap import build_implementation_bootstrap
from accel_pipeline.paths import find_repo_root
from accel_pipeline.tools.repo_tools import ToolContext, make_repo_tools


REPO_ROOT = find_repo_root()


def test_implementation_bootstrap_lists_q18_files_and_references() -> None:
    import json

    analysis_path = REPO_ROOT / "pipeline" / "artifacts" / "tpch" / "q18" / "analysis.json"
    analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
    workload = load_workload("tpch_q18_aux", REPO_ROOT)
    text = build_implementation_bootstrap(REPO_ROOT, workload, 18, analysis)
    assert "module.hpp" in text
    assert "read_reference_files" in text
    assert "accelerators/tpch/q10/duckdb/aux/module.cpp" in text or "q10" in text


def test_implementation_prompt_includes_bootstrap() -> None:
    from accel_pipeline.prompts import render_prompt

    pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
    workload = load_workload("tpch_q18_aux", REPO_ROOT)
    pipeline_config = apply_workload_overrides(pipeline_config, workload)
    prompt = render_prompt(pipeline_config, "02_implementation.md", workload, 18, "implementation")
    assert "Implementation bootstrap" in prompt
    assert "read_reference_files" in prompt


def test_read_reference_files_batches_reads() -> None:
    pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
    tools = make_repo_tools(ToolContext(pipeline_config, 18, stage="implementation"))
    read_reference_files = tools[9]
    out = read_reference_files(
        paths=[
            "pipeline/prompts/aux_state/shared/module_layout.md",
            "accelerators/tpch/q18/config/q18_aux_cost_model.json",
        ],
        limit_per_file=20,
    )
    assert "=== pipeline/prompts/aux_state/shared/module_layout.md ===" in out
    assert "=== accelerators/tpch/q18/config/q18_aux_cost_model.json ===" in out
    assert "Module layout" in out


def test_grep_context_rejects_directory() -> None:
    from accel_pipeline.tools.pipeline_tool_wrapper import wrap_pipeline_tool

    pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
    tools = make_repo_tools(ToolContext(pipeline_config, 18))
    grep_context = tools[8]
    wrapped = wrap_pipeline_tool(grep_context, "grep_context")
    result = wrapped(path="substrate", pattern="AuxQueryModule")
    assert "requires a file path" in result
