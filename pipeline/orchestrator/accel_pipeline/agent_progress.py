from __future__ import annotations

import sys
from contextvars import ContextVar
from typing import Any

_current_turn: ContextVar[int] = ContextVar("_current_turn", default=0)
_current_stage: ContextVar[str] = ContextVar("_current_stage", default="")


def begin_agent_run(
    stage: str,
    *,
    max_turns: int,
    tools: list[str],
    prompt_chars: int,
) -> None:
    _current_stage.set(stage)
    _current_turn.set(0)
    tools_str = ", ".join(tools) if tools else "(none)"
    print(
        f"[pipeline] agent run start: stage={stage} max_turns={max_turns} "
        f"prompt_chars={prompt_chars} tools=[{tools_str}]",
        file=sys.stderr,
        flush=True,
    )


def begin_agent_turn(stage: str, turn: int, *, input_items: int = 0) -> None:
    _current_stage.set(stage)
    _current_turn.set(turn)
    extra = f" input_items={input_items}" if input_items else ""
    print(f"[pipeline] agent turn {turn}: stage={stage}{extra}", file=sys.stderr, flush=True)


def format_tool_args_summary(tool_name: str, args: dict[str, Any]) -> str:
    if tool_name == "run_command" and "command" in args:
        return str(args["command"])[:120]
    if tool_name in {"read_file", "grep_context", "write_file", "file_info"} and "path" in args:
        path = str(args["path"])
        if tool_name == "read_file":
            offset = int(args.get("offset", 0) or 0)
            limit = int(args.get("limit", 0) or 0)
            if offset or limit:
                return f"{path} offset={offset} limit={limit or 'default'}"
        if tool_name == "grep_context" and "pattern" in args:
            return f"{path} pattern={str(args['pattern'])[:60]}"
        return path
    if tool_name in {"search_repo", "rip_grep", "ast_grep"} and "pattern" in args:
        glob = str(args.get("glob", "*"))
        path = str(args.get("path", "."))
        return f"pattern={str(args['pattern'])[:60]} path={path}"
    if tool_name == "list_dir" and "path" in args:
        return str(args["path"])
    if tool_name == "glob_files" and "pattern" in args:
        return str(args["pattern"])
    if tool_name == "read_reference_files" and "paths" in args:
        paths = args["paths"]
        if isinstance(paths, list):
            return f"{len(paths)} files"
    if tool_name == "apply_patch" and "path" in args:
        return str(args["path"])
    compact = ", ".join(f"{k}={str(v)[:40]}" for k, v in list(args.items())[:4])
    return compact[:120]


def log_tool_call(tool_name: str, args: dict[str, Any] | None = None) -> None:
    turn = _current_turn.get()
    stage = _current_stage.get()
    summary = format_tool_args_summary(tool_name, args or {}) if args else ""
    prefix = f"[pipeline] tool_call turn={turn}" if turn else "[pipeline] tool_call"
    if stage:
        prefix += f" stage={stage}"
    line = f"{prefix}: {tool_name}"
    if summary:
        line += f" ({summary})"
    print(line, file=sys.stderr, flush=True)


def log_tool_result(tool_name: str, result: object) -> None:
    turn = _current_turn.get()
    text = str(result).strip().replace("\n", " ")
    if text.startswith("ERROR:"):
        preview = text[:160]
    else:
        preview = f"{len(text)} chars"
        if len(text) <= 120:
            preview = text
        else:
            preview = f"{len(text)} chars: {text[:100]}..."
    prefix = f"[pipeline] tool_result turn={turn}" if turn else "[pipeline] tool_result"
    print(f"{prefix}: {tool_name} -> {preview}", file=sys.stderr, flush=True)


def log_agent_message(stage: str, message: Any) -> None:
    msg_type = getattr(message, "type", "")
    if msg_type == "assistant":
        content = getattr(getattr(message, "message", None), "content", None) or []
        for block in content:
            if getattr(block, "type", "") == "tool_use":
                tool_name = getattr(block, "name", "tool")
                print(f"[pipeline] {stage}: agent requested {tool_name}", file=sys.stderr, flush=True)
