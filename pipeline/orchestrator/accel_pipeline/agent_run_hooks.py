from __future__ import annotations

import json
import sys
from typing import Any

from agents.lifecycle import RunHooks

from accel_pipeline.agent_progress import begin_agent_turn, format_tool_args_summary, log_tool_result


class PipelineLoggingHooks(RunHooks):
    """OpenAI Agents SDK hooks: log model turns and tool results to stderr."""

    def __init__(self, stage: str) -> None:
        self._stage = stage
        self._turn = 0

    async def on_llm_start(self, context: Any, agent: Any, system_prompt: Any, input_items: Any) -> None:
        self._turn += 1
        begin_agent_turn(self._stage, self._turn, input_items=len(input_items or []))

    async def on_tool_end(self, context: Any, agent: Any, tool: Any, result: object) -> None:
        tool_name = getattr(tool, "name", None) or getattr(tool, "__name__", "tool")
        log_tool_result(str(tool_name), result)


def summarize_run_items(new_items: list[Any]) -> dict[str, Any]:
    """Summarize an Agents SDK run for logging."""
    tool_calls: list[str] = []
    assistant_chars = 0
    for item in new_items or []:
        item_type = getattr(item, "type", "")
        if item_type == "tool_call_item":
            name = getattr(item, "tool_name", None) or "tool"
            raw = getattr(item, "raw_item", None)
            args_summary = ""
            if raw is not None:
                arguments = getattr(raw, "arguments", None)
                if arguments:
                    try:
                        parsed = json.loads(arguments)
                        if isinstance(parsed, dict):
                            args_summary = format_tool_args_summary(str(name), parsed)
                    except json.JSONDecodeError:
                        args_summary = str(arguments)[:80]
            line = str(name)
            if args_summary:
                line += f" ({args_summary})"
            tool_calls.append(line)
        elif item_type == "message_output_item":
            raw = getattr(item, "raw_item", None)
            content = getattr(raw, "content", None) if raw is not None else None
            if isinstance(content, list):
                for block in content:
                    text = getattr(block, "text", None)
                    if text:
                        assistant_chars += len(str(text))
            elif content:
                assistant_chars += len(str(content))
    return {
        "tool_call_count": len(tool_calls),
        "tool_calls": tool_calls,
        "assistant_output_chars": assistant_chars,
    }


def log_agent_run_summary(
    stage: str,
    *,
    status: str,
    max_turns: int,
    turns_used: int | None = None,
    new_items: list[Any] | None = None,
) -> None:
    summary = summarize_run_items(new_items or [])
    turns_part = f" turns={turns_used}/{max_turns}" if turns_used is not None else f" max_turns={max_turns}"
    header = (
        f"[pipeline] agent run {status}: stage={stage}{turns_part} "
        f"tool_calls={summary['tool_call_count']} assistant_chars={summary['assistant_output_chars']}"
    )
    print(header, file=sys.stderr, flush=True)
    for idx, call in enumerate(summary["tool_calls"], start=1):
        print(f"[pipeline]   {idx}. {call}", file=sys.stderr, flush=True)
