"""Agent-declared optimization saturation (early stop) helpers."""

from __future__ import annotations

import json
import re
from typing import Any


OPTIMIZATION_SATURATED = "OPTIMIZATION_SATURATED"


def parse_optimization_saturated(text: str) -> dict[str, Any] | None:
    """Return payload if the worker declared OPTIMIZATION_SATURATED, else None."""
    if OPTIMIZATION_SATURATED not in (text or ""):
        return None
    fence = re.search(r"```(?:json)?\s*(\{.*?\})\s*```", text, re.DOTALL)
    payload: dict[str, Any] = {"status": "optimization_saturated"}
    if fence:
        try:
            raw = json.loads(fence.group(1))
            if isinstance(raw, dict):
                payload.update(raw)
        except json.JSONDecodeError:
            pass
    if not payload.get("reason"):
        # Best-effort: line after marker.
        for line in (text or "").splitlines():
            if "reason" in line.lower() and len(line.strip()) > 8:
                payload["reason"] = line.strip()[:240]
                break
    payload.setdefault("reason", "agent declared optimization saturated")
    payload["status"] = "optimization_saturated"
    return payload


def early_stop_config(pipeline_config: Any) -> dict[str, Any]:
    raw = getattr(pipeline_config, "optimization_early_stop", None)
    if isinstance(raw, dict):
        cfg = dict(raw)
    else:
        cfg = {}
    return {"enabled": bool(cfg.get("enabled", True))}


def evaluate_saturation_claim(
    pipeline_config: Any,
    workload: dict[str, Any],
    art: Any,
    payload: dict[str, Any] | None,
) -> tuple[bool, str, dict[str, Any]]:
    """Honor agent OPTIMIZATION_SATURATED claims (no numeric speedup floors).

    Absolute ceiling is query-dependent; the agent decides when further attempts
    are stuck. Orchestrator only gates on ``early_stop.enabled``.
    """
    _ = art, workload
    cfg = early_stop_config(pipeline_config)
    if not cfg["enabled"]:
        return False, "early_stop disabled in config", cfg
    reason = "agent judged optimization saturated"
    if payload and payload.get("reason"):
        reason = str(payload.get("reason"))[:300]
    return True, reason, cfg
