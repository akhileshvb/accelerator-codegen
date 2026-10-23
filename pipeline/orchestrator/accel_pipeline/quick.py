"""The seconds-level inner loop: incremental build, the query's tests, a subset differential.

Workstream C fills run_quick_check.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

from accel_pipeline.failure import Failure


@dataclass
class QuickResult:
    status: str  # "pass" | "fail" | "skip"
    phase_timings: dict[str, float] = field(default_factory=dict)
    failure: Failure | None = None
    summary: str = ""


def run_quick_check(pipeline_config: Any, workload: dict, query: int, *, targets: list[str] | None = None,
                    differential: bool = True) -> QuickResult:
    return QuickResult(status="skip", summary="quick tier not implemented")
