from __future__ import annotations

from typing import Any


def none_default(value: Any, default: Any) -> Any:
    """Return default when an LLM/tool schema passes JSON null for an optional arg."""
    return default if value is None else value

