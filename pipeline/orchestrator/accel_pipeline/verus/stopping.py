"""When the Verus optimization loop stops (pure logic; see config/verus.yaml).

Three layers, so the loop always reaches the audit but cannot quit before it
has explored:

* hard ceilings: the stage's wall-clock budget (each attempt also has its own
  timeout and bounded fix rounds, enforced by the loop);
* progress: ``patience`` consecutive attempts without an accepted improvement,
  or the last ``diminishing_window`` accepts gaining less than
  ``diminishing_min_gain_pct`` together;
* exploration: the progress stop, and an agent's OPTIMIZATION_SATURATED claim,
  count only once every profile hotspot at or above ``hotspot_share`` has had
  an attempt (or was declared infeasible with a reason), both axes (read and
  maintenance) were tried, a structural move was tried or reasoned about once
  progress stalled, and ``min_attempts`` ran.

Counts are in attempts, so the stop can always fire within the budget.
"""

from __future__ import annotations

import time
from dataclasses import asdict, dataclass, field
from typing import Any


@dataclass
class Attempt:
    number: int
    target: str  # the hotspot (or "structural") the attempt aimed at
    axis: str  # "read" | "maintenance" | "both"
    structural: bool
    accepted: bool
    gain_pct: float  # accepted improvement over the previous best (0 if rejected)
    outcome: str  # "accepted" | "rejected: ..."


@dataclass
class StopState:
    started_at: float = field(default_factory=time.time)
    attempts: list[dict[str, Any]] = field(default_factory=list)
    hotspots: list[str] = field(default_factory=list)  # names at or above the share, latest profile
    infeasible: dict[str, str] = field(default_factory=dict)  # hotspot -> reason
    structural_considered: bool = False

    def record(self, a: Attempt) -> None:
        self.attempts.append(asdict(a))


@dataclass
class StopConfig:
    stage_budget_sec: float = 7200
    patience: int = 4
    diminishing_window: int = 3
    diminishing_min_gain_pct: float = 5.0
    min_attempts: int = 3

    @classmethod
    def from_settings(cls, opt: dict[str, Any]) -> "StopConfig":
        return cls(
            stage_budget_sec=float(opt.get("stage_budget_sec", 7200)),
            patience=int(opt.get("patience", 4)),
            diminishing_window=int(opt.get("diminishing_window", 3)),
            diminishing_min_gain_pct=float(opt.get("diminishing_min_gain_pct", 5.0)),
            min_attempts=int(opt.get("min_attempts", 3)),
        )


def uncovered(st: StopState, cfg: StopConfig) -> list[str]:
    """What exploration is still owed before a progress stop may count."""
    owed = []
    tried = {a["target"] for a in st.attempts}
    for h in st.hotspots:
        if h not in tried and h not in st.infeasible:
            owed.append(f"hotspot {h}: no attempt yet (or declare it infeasible with a reason)")
    axes = {a["axis"] for a in st.attempts}
    if not ({"read", "both"} & axes):
        owed.append("no attempt on the read path yet")
    if not ({"maintenance", "both"} & axes):
        owed.append("no attempt on maintenance (writes) yet")
    if len(st.attempts) < cfg.min_attempts:
        owed.append(f"{cfg.min_attempts - len(st.attempts)} more attempt(s) before stopping")
    stalled = _since_last_accept(st) >= 2
    if stalled and not st.structural_considered and not any(a["structural"] for a in st.attempts):
        owed.append("progress stalled: try or reason about a different aux-state design (structural move)")
    return owed


def _since_last_accept(st: StopState) -> int:
    n = 0
    for a in reversed(st.attempts):
        if a["accepted"]:
            break
        n += 1
    return n


def should_stop(st: StopState, cfg: StopConfig, *, saturated_claim: bool = False,
                now: float | None = None) -> tuple[bool, str]:
    """(stop?, why).  The ceiling always stops; progress and claims need exploration."""
    now = time.time() if now is None else now
    if now - st.started_at >= cfg.stage_budget_sec:
        return True, f"stage budget ({cfg.stage_budget_sec:.0f}s) spent"
    progress = ""
    if _since_last_accept(st) >= cfg.patience:
        progress = f"{cfg.patience} attempts without an accepted improvement"
    accepts = [a for a in st.attempts if a["accepted"]]
    if not progress and len(accepts) >= cfg.diminishing_window and _since_last_accept(st) >= 1:
        gain = 1.0
        for a in accepts[-cfg.diminishing_window:]:
            gain *= 1.0 + a["gain_pct"] / 100.0
        if (gain - 1.0) * 100.0 < cfg.diminishing_min_gain_pct:
            progress = (f"the last {cfg.diminishing_window} accepts gained {(gain - 1) * 100:.1f}% together "
                        f"(< {cfg.diminishing_min_gain_pct}%)")
    if saturated_claim and not progress:
        progress = "the agent declared the optimization saturated"
    if not progress:
        return False, ""
    owed = uncovered(st, cfg)
    if owed:
        return False, f"{progress}, but exploration is owed: " + "; ".join(owed)
    return True, progress
