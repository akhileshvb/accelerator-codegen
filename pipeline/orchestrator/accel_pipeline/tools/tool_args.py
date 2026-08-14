from __future__ import annotations

import inspect
from typing import Any


def none_default(value: Any, default: Any) -> Any:
    """Return default when an LLM/tool schema passes JSON null for an optional arg."""
    return default if value is None else value


def coerce_none_defaults(signature: inspect.Signature, arguments: dict[str, Any]) -> dict[str, Any]:
    """Replace explicit None values with parameter defaults from the tool signature."""
    coerced = dict(arguments)
    for name, param in signature.parameters.items():
        if coerced.get(name) is None and param.default is not inspect.Parameter.empty:
            coerced[name] = param.default
    return coerced
