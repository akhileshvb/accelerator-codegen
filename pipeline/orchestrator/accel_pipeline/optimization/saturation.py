"""Agent-declared optimization saturation (early stop) and the evidence it must come with.

A stateful accelerator whose reads are barely faster than the engine's own SQL is not
saturated, whatever the write axis says. Below the read-speedup floor a claim is honored only
after the last N trials were each profile-guided and used distinct strategies, among them one
structural change and one low-level (vectorization / layout) change. If those were all tried and
the floor is still missed, the first such claim triggers one structural re-analysis instead.
"""

from __future__ import annotations

import json
import re
from datetime import datetime, timezone
from enum import Enum
from pathlib import Path
from typing import Any

from accel_pipeline.config import read_json


OPTIMIZATION_SATURATED = "OPTIMIZATION_SATURATED"
TRIAL_PROTOCOL_FILE = "trial_protocol.jsonl"
REANALYSIS_MARKER_FILE = "saturation_reanalysis_requested"
PROFILE_RUN_FILE = "last_profile_run"
BEST_VALID_FILE = "best_valid_baseline.json"

STRATEGY_STRUCTURAL = "structural"
STRATEGY_LOW_LEVEL = "low_level"
STRATEGIES = (STRATEGY_STRUCTURAL, STRATEGY_LOW_LEVEL, "other")
_STRATEGY_HELP = {
    STRATEGY_STRUCTURAL: "a structural change (different state organization, partitioning or algorithm, so the read visits less state)",
    STRATEGY_LOW_LEVEL: "a low-level vectorization/layout change (SIMD with a scalar fallback, branch-free predicates, compact columnar layout, prefetch, no allocation or indirection in the hot loop)",
}

DEFAULT_MIN_READ_SPEEDUP = 20.0
DEFAULT_REQUIRED_ATTEMPTS = 8  # profile-guided, distinct trials before any saturation claim is honored


REDESIGN_HINT = (
    "If the profile shows the state itself is the problem (the read visits too much state, or the layout "
    "defeats SIMD), redesign it in this stage: a strategy=structural trial with a design_revision; it must "
    "pass the full suite and beat the best to stay."
)


class Verdict(Enum):
    HONOR = "honor"
    REJECT = "reject"
    REANALYZE = "reanalyze"


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
    cfg = dict(raw) if isinstance(raw, dict) else {}
    return {
        "enabled": bool(cfg.get("enabled", True)),
        "saturation_min_read_speedup": float(
            cfg.get("saturation_min_read_speedup", DEFAULT_MIN_READ_SPEEDUP)
        ),
        "saturation_required_attempts": int(
            cfg.get("saturation_required_attempts", DEFAULT_REQUIRED_ATTEMPTS)
        ),
    }


def mark_profile_run(art: Path) -> None:
    (art / PROFILE_RUN_FILE).write_text(datetime.now(timezone.utc).isoformat(), encoding="utf-8")


def profile_guidance_error(art: Path, *, hot_spot: str, expected_gain: str, strategy: str) -> str:
    """Why a trial may not begin, or "" when it starts from a fresh profile and names its target."""
    marker, best = art / PROFILE_RUN_FILE, art / BEST_VALID_FILE
    if not marker.is_file() or (best.is_file() and best.stat().st_mtime > marker.stat().st_mtime + 0.01):
        return "run_profile on the current tree first (after an accept or restore the profile is stale)."
    missing = [name for name, value in (("hot_spot", hot_spot), ("expected_gain", expected_gain)) if not value.strip()]
    if missing:
        return f"name {' and '.join(missing)}: the hot function or loop from the run_profile output you target, and the gain you expect there."
    if strategy not in STRATEGIES:
        return f"strategy must be one of {', '.join(STRATEGIES)}."
    return ""


def record_trial_protocol(
    art: Path, *, trial_id: str, hot_spot: str, expected_gain: str, strategy: str
) -> None:
    """One line per A/B trial that began from a profile: what it targeted and how."""
    entry = {
        "trial_id": trial_id,
        "hot_spot": hot_spot.strip(),
        "expected_gain": expected_gain.strip(),
        "strategy": strategy,
    }
    with (art / TRIAL_PROTOCOL_FILE).open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(entry) + "\n")


def load_trial_protocol(art: Path) -> list[dict[str, Any]]:
    path = art / TRIAL_PROTOCOL_FILE
    if not path.is_file():
        return []
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return rows


def best_valid_read_speedup(art: Path) -> float:
    """Read-only speedup over the engine's own SQL of the best valid state."""
    metrics = read_json(art / "best_valid_baseline.json").get("metrics") or {}
    try:
        return float(metrics.get("read_only_query_speedup") or 0.0)
    except (TypeError, ValueError):
        return 0.0


def missing_saturation_work(trials: list[dict[str, Any]], required: int) -> list[str]:
    """What the last `required` profile-guided trials still lack for a saturation claim."""
    recent = trials[-required:] if required > 0 else []
    missing: list[str] = []
    if len(recent) < required:
        missing.append(
            f"{required - len(recent)} more profile-guided trial(s): only {len(recent)} of the "
            f"{required} required began from run_profile output (begin_ab_trial with hot_spot, "
            "expected_gain and strategy)"
        )
    distinct = {(t.get("strategy"), str(t.get("hot_spot", "")).strip().lower()) for t in recent}
    if recent and len(distinct) < len(recent):
        missing.append("distinct strategies: two of the last trials targeted the same hot spot the same way")
    present = {t.get("strategy") for t in recent}
    for strategy in (STRATEGY_STRUCTURAL, STRATEGY_LOW_LEVEL):
        if strategy not in present:
            missing.append(f"one trial with strategy={strategy}: {_STRATEGY_HELP[strategy]}")
    return missing


def judge_saturation_claim(
    pipeline_config: Any,
    art: Path,
    payload: dict[str, Any] | None,
) -> tuple[Verdict, str, dict[str, Any]]:
    """Decide an OPTIMIZATION_SATURATED claim: honor, reject (naming what is missing) or re-analyze."""
    cfg = early_stop_config(pipeline_config)
    if not cfg["enabled"]:
        return Verdict.REJECT, "early_stop disabled in config", cfg
    reason = "agent judged optimization saturated"
    if payload and payload.get("reason"):
        reason = str(payload.get("reason"))[:300]

    floor = cfg["saturation_min_read_speedup"]
    speedup = best_valid_read_speedup(art)
    below_floor = 0 < speedup < floor
    missing = missing_saturation_work(load_trial_protocol(art), cfg["saturation_required_attempts"])
    if missing:
        where = (f"read speedup over the engine's own SQL is {speedup:.2f}x, below the {floor:g}x floor; " if below_floor
                 else "not saturated yet (reads, writes and the state build all still count); ")
        return Verdict.REJECT, where + "still to do: " + "; ".join(missing) + ". " + REDESIGN_HINT, cfg
    if not below_floor:
        return Verdict.HONOR, reason, cfg
    if (art / REANALYSIS_MARKER_FILE).exists():
        return Verdict.HONOR, f"{reason} (read speedup {speedup:.2f}x; structural re-analysis already spent)", cfg
    return (
        Verdict.REANALYZE,
        f"read speedup {speedup:.2f}x is below the {floor:g}x floor after profile-guided structural and "
        f"low-level attempts; {reason}",
        cfg,
    )


def evaluate_saturation_claim(
    pipeline_config: Any,
    workload: dict[str, Any],
    art: Any,
    payload: dict[str, Any] | None,
) -> tuple[bool, str, dict[str, Any]]:
    """Whether the claim ends the stage: (honored, detail, config). A re-analysis request ends it too."""
    _ = workload
    verdict, detail, cfg = judge_saturation_claim(pipeline_config, Path(art), payload)
    return verdict is not Verdict.REJECT, detail, cfg
