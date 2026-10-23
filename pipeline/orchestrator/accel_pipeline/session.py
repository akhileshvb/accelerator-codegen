"""Codex session continuity and wall budgets (workstream A fills this)."""

from __future__ import annotations

from typing import Any


def budget_footer(pipeline_config: Any, stage: str, query: int) -> str:
    """Text appended to every MCP tool result: empty, or a wrap-up/checkpoint notice near the deadline."""
    return ""
