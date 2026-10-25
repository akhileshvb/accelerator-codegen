"""The agent's MCP tools. The tools of a mode live in the mode's own package and are imported only when the mode runs."""

from __future__ import annotations

import importlib
from types import ModuleType
from typing import Any

MODE_TOOLS = {
    "optimization": "accel_pipeline.optimization.tools",
    "engine_backend": "accel_pipeline.engine_backend.tools",
    "pack": "accel_pipeline.pack.tools",
    "verus": "accel_pipeline.verus.tools",
}


def mode_tools(mode: str) -> ModuleType:
    return importlib.import_module(MODE_TOOLS[mode])


def none_default(value: Any, default: Any) -> Any:
    """`default` when an LLM's tool call passes JSON null for an optional argument."""
    return default if value is None else value
