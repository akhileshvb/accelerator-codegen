from __future__ import annotations

import sys
from typing import Any


def log_tool_call(tool_name: str, args: dict[str, Any] | None = None) -> None:
    summary = ""
    if args:
        if tool_name == "run_command" and "command" in args:
            summary = str(args["command"])[:120]
        elif tool_name == "read_file" and "path" in args:
            summary = str(args["path"])
        elif tool_name == "grep_context" and "path" in args:
            summary = str(args["path"])
        elif tool_name == "write_file" and "path" in args:
            summary = str(args["path"])
        elif tool_name == "edit_file" and "path" in args:
            summary = str(args["path"])
    line = f"[pipeline] tool_call: {tool_name}"
    if summary:
        line += f" ({summary})"
    print(line, file=sys.stderr, flush=True)


def log_agent_message(stage: str, message: Any) -> None:
    msg_type = getattr(message, "type", "")
    if msg_type == "assistant":
        content = getattr(getattr(message, "message", None), "content", None) or []
        for block in content:
            if getattr(block, "type", "") == "tool_use":
                tool_name = getattr(block, "name", "tool")
                print(f"[pipeline] {stage}: agent requested {tool_name}", file=sys.stderr, flush=True)
