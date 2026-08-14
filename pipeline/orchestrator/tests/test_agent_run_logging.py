from __future__ import annotations

from types import SimpleNamespace

from accel_pipeline.agent_progress import begin_agent_run, begin_agent_turn, format_tool_args_summary, log_tool_call
from accel_pipeline.agent_run_hooks import log_agent_run_summary, summarize_run_items
from accel_pipeline.tools.tool_policy import STAGE_TOOL_NAMES


def test_analysis_stage_has_write_file() -> None:
    assert "write_file" in STAGE_TOOL_NAMES["analysis"]


def test_optimization_patch_has_write_file() -> None:
    assert "write_file" in STAGE_TOOL_NAMES["optimization_patch"]


def test_optimization_unified_has_search_and_edit_tools() -> None:
    tools = STAGE_TOOL_NAMES["optimization_unified"]
    assert "write_file" in tools
    assert "rip_grep" in tools
    assert "ast_grep" in tools
    assert "read_file" not in tools
    assert "read_reference_files" not in tools
    assert "delete_file" in tools


def test_format_tool_args_summary_read_file() -> None:
    summary = format_tool_args_summary(
        "read_file",
        {"path": "pipeline/prompts/aux_state/shared/taxonomy_index.md", "offset": 0, "limit": 100},
    )
    assert "taxonomy_index.md" in summary
    assert "offset=0" in summary


def test_summarize_run_items_counts_tool_calls() -> None:
    items = [
        SimpleNamespace(
            type="tool_call_item",
            tool_name="read_file",
            raw_item=SimpleNamespace(arguments='{"path":"foo.md"}'),
        ),
        SimpleNamespace(
            type="message_output_item",
            raw_item=SimpleNamespace(content=[SimpleNamespace(text="hello")]),
        ),
    ]
    summary = summarize_run_items(items)
    assert summary["tool_call_count"] == 1
    assert summary["assistant_output_chars"] == 5
    assert "read_file" in summary["tool_calls"][0]


def test_agent_run_logging(capsys) -> None:
    begin_agent_run("analysis", max_turns=8, tools=["read_file", "write_file"], prompt_chars=12000)
    begin_agent_turn("analysis", 1, input_items=2)
    log_tool_call("read_file", {"path": "pipeline/schemas/aux_state_analysis.schema.json"})
    captured = capsys.readouterr()
    assert "agent run start" in captured.err
    assert "prompt_chars=12000" in captured.err
    assert "agent turn 1" in captured.err
    assert "tool_call turn=1 stage=analysis" in captured.err
    assert "aux_state_analysis.schema.json" in captured.err


def test_log_agent_run_summary(capsys) -> None:
    items = [
        SimpleNamespace(
            type="tool_call_item",
            tool_name="search_repo",
            raw_item=SimpleNamespace(arguments='{"pattern":"AuxQueryModule","glob":"*.hpp"}'),
        ),
    ]
    log_agent_run_summary(
        "analysis",
        status="max_turns_exceeded",
        max_turns=8,
        turns_used=8,
        new_items=items,
    )
    captured = capsys.readouterr()
    assert "max_turns_exceeded" in captured.err
    assert "turns=8/8" in captured.err
    assert "search_repo" in captured.err
