from __future__ import annotations

import functools
import inspect
import json
from collections.abc import Callable
from pathlib import Path
from typing import Any

from accel_pipeline.agent_progress import log_tool_call
from accel_pipeline.optimization_runtime import compress_tool_output
from accel_pipeline.tools.tool_args import coerce_none_defaults


def wrap_pipeline_tool(
    fn: Callable[..., Any],
    tool_name: str,
    *,
    art_dir: Path | None = None,
    compress_outputs: bool = False,
) -> Callable[..., str]:
    """Wrap a pipeline tool with logging, error handling, and optional output compression."""
    signature = inspect.signature(fn)

    @functools.wraps(fn)
    def wrapped(*args: Any, **kwargs: Any) -> str:
        bound = signature.bind_partial(*args, **kwargs)
        bound.apply_defaults()
        call_args = coerce_none_defaults(signature, dict(bound.arguments))
        log_tool_call(tool_name, call_args)
        try:
            result = fn(**call_args)
            if not isinstance(result, str):
                result = json.dumps(result, indent=2)
            if compress_outputs and art_dir is not None:
                result = compress_tool_output(art_dir, tool_name, result)
            return result
        except Exception as exc:  # noqa: BLE001 - return tool errors to the agent
            return f"ERROR: {type(exc).__name__}: {exc}"

    return wrapped


def wrap_tool_registry(
    registry: dict[str, Callable[..., Any]],
    *,
    art_dir: Path | None = None,
    compress_outputs: bool = False,
) -> dict[str, Callable[..., str]]:
    return {
        name: wrap_pipeline_tool(fn, name, art_dir=art_dir, compress_outputs=compress_outputs)
        for name, fn in registry.items()
    }
