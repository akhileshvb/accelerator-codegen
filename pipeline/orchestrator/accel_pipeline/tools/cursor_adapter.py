from __future__ import annotations

import json
from typing import Any, Callable

from pathlib import Path

from accel_pipeline.agent_progress import log_tool_call
from accel_pipeline.optimization_runtime import compress_tool_output
from accel_pipeline.tools.schemas import TOOL_SCHEMAS

try:
    from cursor_sdk import CustomTool
except ImportError:  # pragma: no cover - optional dependency
    CustomTool = None  # type: ignore[misc, assignment]


def _tool_result(value: Any) -> str:
    if isinstance(value, str):
        return value
    return json.dumps(value, indent=2)


def _make_executor(
    fn: Callable[..., Any],
    tool_name: str,
    *,
    art_dir: Path | None = None,
    compress_outputs: bool = False,
) -> Callable[[dict[str, Any], Any], str]:
    def execute(args: dict[str, Any], _context: Any) -> str:
        log_tool_call(tool_name, args)
        try:
            result = _tool_result(fn(**args))
            if compress_outputs and art_dir is not None:
                result = compress_tool_output(art_dir, tool_name, result)
            return result
        except Exception as exc:  # noqa: BLE001 - return tool errors to the agent
            return f"ERROR: {type(exc).__name__}: {exc}"

    return execute


def build_cursor_custom_tools(
    registry: dict[str, Callable[..., Any]],
    *,
    art_dir: Path | None = None,
    compress_outputs: bool = False,
) -> dict[str, Any]:
    if CustomTool is None:
        raise ImportError(
            "cursor-sdk is not installed; install with: pip install 'accel-pipeline[cursor]'"
        )

    custom_tools: dict[str, Any] = {}
    for name, fn in registry.items():
        schema = TOOL_SCHEMAS.get(name)
        if schema is None:
            raise KeyError(f"missing JSON schema for tool: {name}")
        description = (fn.__doc__ or name).strip()
        custom_tools[name] = CustomTool(
            description=description,
            input_schema=schema,
            execute=_make_executor(fn, name, art_dir=art_dir, compress_outputs=compress_outputs),
        )
    return custom_tools
