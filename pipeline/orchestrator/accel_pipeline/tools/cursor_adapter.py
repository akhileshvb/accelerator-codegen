from __future__ import annotations

from typing import Any, Callable

from pathlib import Path

from accel_pipeline.tools.pipeline_tool_wrapper import wrap_pipeline_tool
from accel_pipeline.tools.schemas import TOOL_SCHEMAS

try:
    from cursor_sdk import CustomTool
except ImportError:  # pragma: no cover - optional dependency
    CustomTool = None  # type: ignore[misc, assignment]


def _make_executor(
    fn: Callable[..., Any],
    tool_name: str,
    *,
    art_dir: Path | None = None,
    compress_outputs: bool = False,
) -> Callable[[dict[str, Any], Any], str]:
    wrapped = wrap_pipeline_tool(
        fn,
        tool_name,
        art_dir=art_dir,
        compress_outputs=compress_outputs,
    )

    def execute(args: dict[str, Any], _context: Any) -> str:
        return wrapped(**args)

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
